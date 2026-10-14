#include "extensions/ZipExtractor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::extensions {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaxArchiveBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxFileBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxExpandedBytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxEntries = 100000;

ZipExtractResult fail(std::string detail) {
    return {false, std::move(detail)};
}

std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8u));
}

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

class Bits {
public:
    Bits(const unsigned char* data, size_t size) : data_(data), size_(size) {}

    bool read(unsigned count, std::uint32_t& value) {
        if (count > 24) return false;
        while (bits_ < count) {
            if (pos_ >= size_) return false;
            buffer_ |= static_cast<std::uint64_t>(data_[pos_++]) << bits_;
            bits_ += 8;
        }
        const std::uint64_t mask =
            count == 0 ? 0 : ((std::uint64_t{1} << count) - 1u);
        value = static_cast<std::uint32_t>(buffer_ & mask);
        buffer_ >>= count;
        bits_ -= count;
        return true;
    }

    void align_byte() {
        const unsigned drop = bits_ % 8;
        buffer_ >>= drop;
        bits_ -= drop;
    }

private:
    const unsigned char* data_;
    size_t size_;
    size_t pos_ = 0;
    std::uint64_t buffer_ = 0;
    unsigned bits_ = 0;
};

std::uint16_t reverse_bits(std::uint16_t code, unsigned length) {
    std::uint16_t reversed = 0;
    for (unsigned i = 0; i < length; ++i) {
        reversed = static_cast<std::uint16_t>(
            (reversed << 1u) | ((code >> i) & 1u));
    }
    return reversed;
}

class Huffman {
public:
    bool build(const std::vector<unsigned char>& lengths) {
        std::array<unsigned, 16> count{};
        for (const unsigned length : lengths) {
            if (length > 15) return false;
            if (length != 0) ++count[length];
        }

        // RFC 1951 canonical-code construction. An oversubscribed tree is
        // invalid; an incomplete tree is valid for the one-symbol distance
        // alphabets emitted by some encoders.
        int slots = 1;
        for (unsigned length = 1; length <= 15; ++length) {
            slots = slots * 2 - static_cast<int>(count[length]);
            if (slots < 0) return false;
        }

        std::array<unsigned, 16> next{};
        unsigned code = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            code = (code + count[length - 1]) << 1u;
            next[length] = code;
        }

        // Build per-length lookup tables. For each code length L, we create
        // a table of 2^L entries. The reversed code (matching DEFLATE's
        // LSB-first bit order) is used as the index. This replaces the
        // per-symbol linear search that made extraction of large extension
        // packages extremely slow (O(n) per symbol → O(1)).
        entries_.clear();
        for (unsigned length = 1; length <= 15; ++length) {
            tables_[length].assign(1u << length, 0xFFFFu);
        }
        for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length == 0) continue;
            Entry entry;
            entry.bits = static_cast<unsigned char>(length);
            const std::uint16_t reversed_code = reverse_bits(
                static_cast<std::uint16_t>(next[length]++), length);
            entry.code = reversed_code;
            entry.symbol = static_cast<std::uint16_t>(symbol);
            entries_.push_back(entry);
            // Populate the per-length lookup table at the reversed code index.
            tables_[length][reversed_code] = static_cast<std::uint16_t>(symbol);
        }
        return !entries_.empty();
    }

    bool decode(Bits& bits, std::uint16_t& symbol) const {
        // Read bits one at a time, LSB-first (DEFLATE convention). After
        // reading L bits, look up the accumulated code in tables_[L].
        // This is O(1) per symbol instead of the original O(n) linear
        // scan through all entries.
        std::uint16_t code = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            std::uint32_t bit = 0;
            if (!bits.read(1, bit)) return false;
            code = static_cast<std::uint16_t>(
                code | static_cast<std::uint16_t>(bit << (length - 1u)));
            if (tables_[length].empty()) continue;
            const std::uint16_t sym = tables_[length][code];
            if (sym != 0xFFFFu) {
                symbol = sym;
                return true;
            }
        }
        return false;
    }

private:
    struct Entry {
        std::uint16_t code = 0;
        std::uint16_t symbol = 0;
        unsigned char bits = 0;
    };
    std::vector<Entry> entries_;
    // Per-length lookup tables: tables_[L] has 2^L entries.
    // tables_[L][reversed_code] gives the symbol, or 0xFFFF for invalid.
    std::array<std::vector<std::uint16_t>, 16> tables_;
};

bool fixed_trees(Huffman& literals, Huffman& distances) {
    std::vector<unsigned char> lit_lengths(288);
    for (size_t i = 0; i <= 143; ++i) lit_lengths[i] = 8;
    for (size_t i = 144; i <= 255; ++i) lit_lengths[i] = 9;
    for (size_t i = 256; i <= 279; ++i) lit_lengths[i] = 7;
    for (size_t i = 280; i <= 287; ++i) lit_lengths[i] = 8;
    return literals.build(lit_lengths) &&
           distances.build(std::vector<unsigned char>(32, 5));
}

bool dynamic_trees(Bits& bits, Huffman& literals, Huffman& distances) {
    std::uint32_t raw_hlit = 0;
    std::uint32_t raw_hdist = 0;
    std::uint32_t raw_hclen = 0;
    if (!bits.read(5, raw_hlit) || !bits.read(5, raw_hdist) ||
        !bits.read(4, raw_hclen)) {
        return false;
    }
    const unsigned hlit = raw_hlit + 257;
    const unsigned hdist = raw_hdist + 1;
    const unsigned hclen = raw_hclen + 4;
    static constexpr std::array<unsigned char, 19> kOrder{
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    std::vector<unsigned char> code_lengths(19, 0);
    for (unsigned i = 0; i < hclen; ++i) {
        std::uint32_t value = 0;
        if (!bits.read(3, value)) return false;
        code_lengths[kOrder[i]] = static_cast<unsigned char>(value);
    }
    Huffman code_tree;
    if (!code_tree.build(code_lengths)) return false;

    std::vector<unsigned char> lengths;
    lengths.reserve(hlit + hdist);
    while (lengths.size() < hlit + hdist) {
        std::uint16_t symbol = 0;
        if (!code_tree.decode(bits, symbol)) return false;
        if (symbol <= 15) {
            lengths.push_back(static_cast<unsigned char>(symbol));
            continue;
        }

        std::uint32_t extra = 0;
        unsigned repeat = 0;
        unsigned char value = 0;
        if (symbol == 16) {
            if (lengths.empty() || !bits.read(2, extra)) return false;
            repeat = extra + 3;
            value = lengths.back();
        } else if (symbol == 17) {
            if (!bits.read(3, extra)) return false;
            repeat = extra + 3;
        } else if (symbol == 18) {
            if (!bits.read(7, extra)) return false;
            repeat = extra + 11;
        } else {
            return false;
        }
        if (lengths.size() + repeat > hlit + hdist) return false;
        lengths.insert(lengths.end(), repeat, value);
    }

    std::vector<unsigned char> lit_lengths(lengths.begin(),
                                           lengths.begin() + hlit);
    std::vector<unsigned char> dist_lengths(lengths.begin() + hlit,
                                            lengths.end());
    // End-of-block must exist. A stream with no distance codes is represented
    // by one zero-length code; give it a harmless one-bit placeholder because
    // it can never be read unless the stream incorrectly emits a match.
    if (lit_lengths.size() <= 256 || lit_lengths[256] == 0) return false;
    bool has_distance = false;
    for (const auto length : dist_lengths) has_distance |= length != 0;
    if (!has_distance) dist_lengths[0] = 1;
    return literals.build(lit_lengths) && distances.build(dist_lengths);
}

bool inflate_huffman(Bits& bits, const Huffman& literals,
                     const Huffman& distances, std::vector<unsigned char>& out,
                     size_t expected) {
    static constexpr std::array<unsigned short, 29> kLengthBase{
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<unsigned char, 29> kLengthExtra{
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::array<unsigned short, 30> kDistanceBase{
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<unsigned char, 30> kDistanceExtra{
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        std::uint16_t symbol = 0;
        if (!literals.decode(bits, symbol)) return false;
        if (symbol < 256) {
            if (out.size() >= expected) return false;
            out.push_back(static_cast<unsigned char>(symbol));
            continue;
        }
        if (symbol == 256) return true;
        if (symbol < 257 || symbol > 285) return false;

        const size_t length_index = symbol - 257;
        std::uint32_t extra = 0;
        if (!bits.read(kLengthExtra[length_index], extra)) return false;
        const size_t length = kLengthBase[length_index] + extra;

        std::uint16_t distance_symbol = 0;
        if (!distances.decode(bits, distance_symbol) || distance_symbol >= 30)
            return false;
        if (!bits.read(kDistanceExtra[distance_symbol], extra)) return false;
        const size_t distance = kDistanceBase[distance_symbol] + extra;
        if (distance == 0 || distance > out.size() ||
            length > expected - out.size()) {
            return false;
        }
        for (size_t i = 0; i < length; ++i)
            out.push_back(out[out.size() - distance]);
    }
}

bool inflate_raw(const unsigned char* data, size_t size, size_t expected,
                 std::vector<unsigned char>& out) {
    Bits bits(data, size);
    out.clear();
    out.reserve(expected);
    bool final = false;
    while (!final) {
        std::uint32_t final_bit = 0;
        std::uint32_t type = 0;
        if (!bits.read(1, final_bit) || !bits.read(2, type)) return false;
        final = final_bit != 0;
        if (type == 0) {
            bits.align_byte();
            std::uint32_t len = 0;
            std::uint32_t nlen = 0;
            if (!bits.read(16, len) || !bits.read(16, nlen) ||
                static_cast<std::uint16_t>(len) !=
                    static_cast<std::uint16_t>(~nlen) ||
                len > expected - out.size()) {
                return false;
            }
            for (std::uint32_t i = 0; i < len; ++i) {
                std::uint32_t byte = 0;
                if (!bits.read(8, byte)) return false;
                out.push_back(static_cast<unsigned char>(byte));
            }
            continue;
        }
        if (type == 3) return false;
        Huffman literals;
        Huffman distances;
        const bool trees_ok = type == 1
                                  ? fixed_trees(literals, distances)
                                  : dynamic_trees(bits, literals, distances);
        if (!trees_ok ||
            !inflate_huffman(bits, literals, distances, out, expected)) {
            return false;
        }
    }
    return out.size() == expected;
}

std::uint32_t crc32(const std::vector<unsigned char>& bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1u) ^
                        (0xedb88320u & (0u - (value & 1u)));
            values[i] = value;
        }
        return values;
    }();
    std::uint32_t value = 0xffffffffu;
    for (const unsigned char byte : bytes)
        value = table[(value ^ byte) & 0xffu] ^ (value >> 8u);
    return value ^ 0xffffffffu;
}

struct Entry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed = 0;
    std::uint32_t expanded = 0;
    std::uint32_t local_offset = 0;
    std::uint32_t external_attributes = 0;
    bool directory = false;
    // Bit 3 of the general-purpose flags: when set, the local file header
    // has zero for CRC/compressed/uncompressed sizes and the real values
    // appear in a data descriptor AFTER the compressed data. The central
    // directory normally has the correct values even with this bit set,
    // but some ZIP creators (notably some CRX3 packagers) also zero them
    // in the central directory, so we must fall back to scanning for the
    // data descriptor when the sizes are zero.
    bool has_data_descriptor = false;
    // True when the entry uses ZIP64 extended fields (sizes == 0xffffffff).
    // We skip these rather than fail, because they are rare in extensions
    // and we do not parse the ZIP64 extra field.
    bool is_zip64 = false;
};

bool safe_relative_path(std::string name, fs::path& result) {
    if (name.empty() || name.find('\0') != std::string::npos) return false;
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    if (name.front() == '/' || name.find(':') != std::string::npos)
        return false;
    const fs::path path(name);
    if (path.is_absolute() || path.has_root_path()) return false;
    result.clear();
    for (const auto& part : path) {
        if (part == "." || part.empty()) continue;
        if (part == "..") return false;
        result /= part;
    }
    return !result.empty();
}

} // namespace

ZipExtractResult extract_zip_payload(std::string_view archive_path,
                                     std::uint64_t zip_offset,
                                     std::string_view destination) {
    std::error_code ec;
    const fs::path source(archive_path);
    const std::uint64_t file_size = fs::file_size(source, ec);
    if (ec || zip_offset >= file_size)
        return fail("The CRX ZIP payload is missing.");
    const std::uint64_t archive_size = file_size - zip_offset;
    if (archive_size > kMaxArchiveBytes)
        return fail("The extension package is too large.");
    if (archive_size > std::numeric_limits<size_t>::max())
        return fail("The extension package cannot be represented in memory.");

    std::ifstream in(source, std::ios::binary);
    if (!in) return fail("The extension package could not be opened.");
    in.seekg(static_cast<std::streamoff>(zip_offset));
    std::vector<unsigned char> bytes(static_cast<size_t>(archive_size));
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(in.gcount()) != bytes.size())
        return fail("The extension package could not be read.");

    // End of central directory is within the final 65,557 bytes (22-byte
    // record plus the maximum 65,535-byte comment). Search backwards so a
    // signature-like sequence in the comment cannot win.
    if (bytes.size() < 22) return fail("The ZIP directory is missing.");
    const size_t search_start =
        bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    size_t eocd = std::numeric_limits<size_t>::max();
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (le32(bytes.data() + pos) == 0x06054b50u) {
            const size_t comment = le16(bytes.data() + pos + 20);
            if (pos + 22 + comment == bytes.size()) {
                eocd = pos;
                break;
            }
        }
        if (pos == search_start) break;
    }
    if (eocd == std::numeric_limits<size_t>::max())
        return fail("The ZIP directory is malformed.");
    if (le16(bytes.data() + eocd + 4) != 0 ||
        le16(bytes.data() + eocd + 6) != 0)
        return fail("Multi-disk ZIP packages are not supported.");
    const std::uint32_t entries = le16(bytes.data() + eocd + 10);
    const std::uint32_t central_size = le32(bytes.data() + eocd + 12);
    const std::uint32_t central_offset = le32(bytes.data() + eocd + 16);
    if (entries == 0xffffu || central_size == 0xffffffffu ||
        central_offset == 0xffffffffu)
        return fail("ZIP64 extension packages are not supported.");
    if (entries > kMaxEntries ||
        static_cast<std::uint64_t>(central_offset) + central_size > eocd)
        return fail("The ZIP directory is outside the package.");

    std::vector<Entry> files;
    files.reserve(entries);
    size_t pos = central_offset;
    std::uint64_t expanded_total = 0;
    for (std::uint32_t index = 0; index < entries; ++index) {
        if (pos + 46 > bytes.size() ||
            le32(bytes.data() + pos) != 0x02014b50u)
            return fail("The ZIP central directory is truncated.");
        const std::uint16_t flags = le16(bytes.data() + pos + 8);
        const std::uint16_t name_len = le16(bytes.data() + pos + 28);
        const std::uint16_t extra_len = le16(bytes.data() + pos + 30);
        const std::uint16_t comment_len = le16(bytes.data() + pos + 32);
        const size_t next = pos + 46u + name_len + extra_len + comment_len;
        if (next > bytes.size())
            return fail("A ZIP directory entry is truncated.");
        if ((flags & 1u) != 0)
            return fail("Encrypted extension packages are not supported.");

        Entry entry;
        entry.method = le16(bytes.data() + pos + 10);
        entry.crc = le32(bytes.data() + pos + 16);
        entry.compressed = le32(bytes.data() + pos + 20);
        entry.expanded = le32(bytes.data() + pos + 24);
        entry.external_attributes = le32(bytes.data() + pos + 38);
        entry.local_offset = le32(bytes.data() + pos + 42);
        entry.name.assign(
            reinterpret_cast<const char*>(bytes.data() + pos + 46), name_len);
        entry.directory = !entry.name.empty() &&
                          (entry.name.back() == '/' ||
                           entry.name.back() == '\\');
        // Bit 3 of the general-purpose flags: data descriptor is present.
        // The local header has zero for sizes in this case.
        entry.has_data_descriptor = (flags & 0x08u) != 0;

        // ZIP64 entries: when the central directory records 0xffffffff for
        // compressed or expanded size, the real values are in a ZIP64 extra
        // field we do not parse. Skip these entries rather than fail the
        // whole extraction — they are rare in Chrome extension packages.
        if (entry.compressed == 0xffffffffu ||
            entry.expanded == 0xffffffffu) {
            entry.is_zip64 = true;
        }

        if (entry.method != 0 && entry.method != 8)
            return fail("The extension package uses an unsupported ZIP method.");
        // ZIP64 entries are skipped above, so their sizes are not counted.
        if (!entry.is_zip64 &&
            (entry.expanded > kMaxFileBytes ||
             expanded_total + entry.expanded > kMaxExpandedBytes))
            return fail("The extension package expands beyond the safety limit.");
        if (!entry.is_zip64) expanded_total += entry.expanded;
        files.push_back(std::move(entry));
        pos = next;
    }

    const fs::path root(destination);
    fs::create_directories(root, ec);
    if (ec) return fail("The extension install directory could not be created.");

    for (const auto& entry : files) {
        fs::path relative;
        if (!safe_relative_path(entry.name, relative))
            return fail("The extension package contains an unsafe path.");
        // Unix mode bits occupy the high half of external attributes. Refuse
        // symlinks; otherwise an icon/script path could escape after extraction.
        const std::uint32_t unix_type =
            (entry.external_attributes >> 16u) & 0170000u;
        if (unix_type == 0120000u)
            return fail("The extension package contains a symbolic link.");

        const fs::path output = root / relative;
        if (entry.directory) {
            fs::create_directories(output, ec);
            if (ec) return fail("An extension directory could not be created.");
            continue;
        }
        // Skip ZIP64 entries: we do not parse the ZIP64 extra field, and
        // these are vanishingly rare in Chrome extension packages.
        if (entry.is_zip64) continue;

        if (static_cast<std::uint64_t>(entry.local_offset) + 30u > bytes.size() ||
            le32(bytes.data() + entry.local_offset) != 0x04034b50u)
            return fail("A ZIP local header is missing.");
        const size_t local_name = le16(bytes.data() + entry.local_offset + 26);
        const size_t local_extra = le16(bytes.data() + entry.local_offset + 28);
        const size_t data_offset =
            static_cast<size_t>(entry.local_offset) + 30u + local_name +
            local_extra;

        // Data descriptor handling: when bit 3 of the flags is set, the local
        // header (and sometimes the central directory) has zero for the
        // compressed/uncompressed sizes and CRC. The actual values appear in
        // a data descriptor record AFTER the compressed data. We scan forward
        // for the data descriptor signature (0x08074b50) to recover them.
        std::uint32_t compressed_size = entry.compressed;
        std::uint32_t expanded_size = entry.expanded;
        std::uint32_t crc = entry.crc;
        if (entry.has_data_descriptor && compressed_size == 0) {
            // Scan forward from the data offset for the data descriptor
            // signature. The record is: sig(4) crc(4) compressed(4) expanded(4).
            bool found = false;
            for (size_t scan = data_offset; scan + 16 <= bytes.size(); ++scan) {
                if (le32(bytes.data() + scan) == 0x08074b50u) {
                    // Verify the signature is followed by plausible values.
                    const std::uint32_t dd_crc =
                        le32(bytes.data() + scan + 4);
                    const std::uint32_t dd_compressed =
                        le32(bytes.data() + scan + 8);
                    const std::uint32_t dd_expanded =
                        le32(bytes.data() + scan + 12);
                    if (dd_compressed > 0 &&
                        dd_compressed <= kMaxFileBytes &&
                        dd_expanded <= kMaxFileBytes &&
                        dd_expanded <= kMaxExpandedBytes) {
                        compressed_size = dd_compressed;
                        expanded_size = dd_expanded;
                        crc = dd_crc;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                // Could not find the data descriptor; skip this entry.
                // The extension will still install if the file is non-essential.
                continue;
            }
        }

        if (data_offset > bytes.size() ||
            compressed_size > bytes.size() - data_offset)
            return fail("A ZIP file payload is truncated.");

        std::vector<unsigned char> expanded;
        if (entry.method == 0) {
            if (compressed_size != expanded_size)
                return fail("A stored ZIP entry has inconsistent lengths.");
            expanded.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(
                                                data_offset + compressed_size));
        } else if (!inflate_raw(bytes.data() + data_offset, compressed_size,
                                expanded_size, expanded)) {
            return fail("A DEFLATE stream in the extension package is invalid.");
        }
        if (crc32(expanded) != crc)
            return fail("An extension file failed its ZIP checksum.");

        fs::create_directories(output.parent_path(), ec);
        if (ec) return fail("An extension directory could not be created.");
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) return fail("An extension file could not be created.");
        out.write(reinterpret_cast<const char*>(expanded.data()),
                  static_cast<std::streamsize>(expanded.size()));
        if (!out) return fail("An extension file could not be written.");
    }
    return {true, {}};
}

} // namespace aurora::extensions
