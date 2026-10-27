#include "inflate.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace aurora::setup {
namespace {

class Bits {
public:
    Bits(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

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
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
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

        for (unsigned length = 1; length <= 15; ++length) {
            tables_[length].assign(1u << length, 0xFFFFu);
        }
        bool any = false;
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length == 0) continue;
            const std::uint16_t reversed = reverse_bits(
                static_cast<std::uint16_t>(next[length]++), length);
            tables_[length][reversed] = static_cast<std::uint16_t>(symbol);
            any = true;
        }
        return any;
    }

    bool decode(Bits& bits, std::uint16_t& symbol) const {
        std::uint16_t code = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            std::uint32_t bit = 0;
            if (!bits.read(1, bit)) return false;
            code = static_cast<std::uint16_t>(
                code | static_cast<std::uint16_t>(bit << (length - 1u)));
            if (tables_[length].empty()) continue;
            const std::uint16_t found = tables_[length][code];
            if (found != 0xFFFFu) {
                symbol = found;
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::vector<std::uint16_t>, 16> tables_;
};

bool fixed_trees(Huffman& literals, Huffman& distances) {
    std::vector<unsigned char> literal_lengths(288);
    for (std::size_t i = 0; i <= 143; ++i) literal_lengths[i] = 8;
    for (std::size_t i = 144; i <= 255; ++i) literal_lengths[i] = 9;
    for (std::size_t i = 256; i <= 279; ++i) literal_lengths[i] = 7;
    for (std::size_t i = 280; i <= 287; ++i) literal_lengths[i] = 8;
    return literals.build(literal_lengths) &&
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

    std::vector<unsigned char> literal_lengths(lengths.begin(),
                                               lengths.begin() + hlit);
    std::vector<unsigned char> distance_lengths(lengths.begin() + hlit,
                                                lengths.end());
    if (literal_lengths.size() <= 256 || literal_lengths[256] == 0) return false;
    bool has_distance = false;
    for (const unsigned char length : distance_lengths) {
        has_distance = has_distance || length != 0;
    }
    if (!has_distance) distance_lengths[0] = 1;
    return literals.build(literal_lengths) && distances.build(distance_lengths);
}

class Stream {
public:
    Stream(InflateSink& sink, std::uint64_t expected)
        : sink_(sink), expected_(expected) {}

    bool emit(std::uint8_t byte) {
        if (produced_ >= expected_) return false;
        window_[static_cast<std::size_t>(produced_ & 32767u)] = byte;
        output_[output_len_++] = byte;
        ++produced_;
        if (output_len_ == output_.size()) return flush();
        return true;
    }

    bool copy(std::size_t distance, std::size_t length) {
        if (distance == 0 || distance > 32768 ||
            distance > static_cast<std::size_t>(produced_) ||
            length > expected_ - produced_) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint8_t byte = window_[static_cast<std::size_t>(
                (produced_ - distance) & 32767u)];
            if (!emit(byte)) return false;
        }
        return true;
    }

    bool flush() {
        if (output_len_ == 0) return true;
        if (!sink_.write(output_.data(), output_len_)) return false;
        output_len_ = 0;
        return true;
    }

    bool finish() {
        if (!flush()) return false;
        return produced_ == expected_;
    }

    std::uint64_t produced() const { return produced_; }
    bool sink_failed() const { return sink_.error != S_OK; }

private:
    InflateSink& sink_;
    std::uint64_t expected_;
    std::array<std::uint8_t, 32768> window_{};
    std::array<std::uint8_t, 65536> output_{};
    std::size_t output_len_ = 0;
    std::uint64_t produced_ = 0;
};

bool inflate_huffman(Bits& bits, const Huffman& literals,
                     const Huffman& distances, Stream& stream) {
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
            if (!stream.emit(static_cast<std::uint8_t>(symbol))) return false;
            continue;
        }
        if (symbol == 256) return true;
        if (symbol < 257 || symbol > 285) return false;

        const std::size_t length_index = symbol - 257;
        std::uint32_t extra = 0;
        if (!bits.read(kLengthExtra[length_index], extra)) return false;
        const std::size_t length = kLengthBase[length_index] + extra;

        std::uint16_t distance_symbol = 0;
        if (!distances.decode(bits, distance_symbol) ||
            distance_symbol >= 30) {
            return false;
        }
        if (!bits.read(kDistanceExtra[distance_symbol], extra)) return false;
        const std::size_t distance = kDistanceBase[distance_symbol] + extra;
        if (!stream.copy(distance, length)) return false;
    }
}

} // namespace

InflateResult inflate_raw_stream(const std::uint8_t* data, std::size_t size,
                                 std::uint64_t expected, InflateSink& sink) {
    InflateResult result;
    Bits bits(data, size);
    Stream stream(sink, expected);
    Huffman fixed_literals;
    Huffman fixed_distances;
    bool fixed_ready = false;
    bool final = false;

    while (!final) {
        std::uint32_t final_bit = 0;
        std::uint32_t type = 0;
        if (!bits.read(1, final_bit) || !bits.read(2, type)) {
            result.error = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
            result.detail = L"The compressed data is truncated.";
            result.produced = stream.produced();
            return result;
        }
        final = final_bit != 0;
        if (type == 0) {
            bits.align_byte();
            std::uint32_t len = 0;
            std::uint32_t nlen = 0;
            if (!bits.read(16, len) || !bits.read(16, nlen) ||
                static_cast<std::uint16_t>(len) !=
                    static_cast<std::uint16_t>(~nlen)) {
                result.error = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
                result.detail = L"The stored block header is invalid.";
                result.produced = stream.produced();
                return result;
            }
            for (std::uint32_t i = 0; i < len; ++i) {
                std::uint32_t byte = 0;
                if (!bits.read(8, byte) ||
                    !stream.emit(static_cast<std::uint8_t>(byte))) {
                    result.error = stream.sink_failed()
                                       ? sink.error
                                       : HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
                    result.detail = stream.sink_failed()
                                        ? sink.error_detail
                                        : L"The stored block is truncated.";
                    result.produced = stream.produced();
                    return result;
                }
            }
            continue;
        }
        if (type == 3) {
            result.error = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
            result.detail = L"The compressed stream uses an invalid block type.";
            result.produced = stream.produced();
            return result;
        }

        const Huffman* literals = nullptr;
        const Huffman* distances = nullptr;
        if (type == 1) {
            if (!fixed_ready &&
                !fixed_trees(fixed_literals, fixed_distances)) {
                result.error = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
                result.detail = L"The fixed Huffman tree is invalid.";
                result.produced = stream.produced();
                return result;
            }
            fixed_ready = true;
            literals = &fixed_literals;
            distances = &fixed_distances;
        } else {
            Huffman dynamic_literals;
            Huffman dynamic_distances;
            if (!dynamic_trees(bits, dynamic_literals, dynamic_distances)) {
                result.error = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
                result.detail = L"The dynamic Huffman tree is invalid.";
                result.produced = stream.produced();
                return result;
            }
            literals = &dynamic_literals;
            distances = &dynamic_distances;
            if (!inflate_huffman(bits, *literals, *distances, stream)) {
                result.error = stream.sink_failed()
                                   ? sink.error
                                   : HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
                result.detail = stream.sink_failed()
                                    ? sink.error_detail
                                    : L"The compressed data is invalid.";
                result.produced = stream.produced();
                return result;
            }
            continue;
        }
        if (!inflate_huffman(bits, *literals, *distances, stream)) {
            result.error = stream.sink_failed()
                               ? sink.error
                               : HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
            result.detail = stream.sink_failed()
                                ? sink.error_detail
                                : L"The compressed data is invalid.";
            result.produced = stream.produced();
            return result;
        }
    }

    if (!stream.finish()) {
        result.error = stream.sink_failed()
                           ? sink.error
                           : HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
        result.detail = stream.sink_failed()
                            ? sink.error_detail
                            : L"The decompressed size does not match the ZIP directory.";
        result.produced = stream.produced();
        return result;
    }
    result.ok = true;
    result.produced = stream.produced();
    return result;
}

} // namespace aurora::setup
