#include "payload.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "inflate.hpp"
#include "win_util.hpp"

namespace aurora::setup {
namespace {

constexpr char kFooterMagic[8] = {'A', 'U', 'R', 'S', 'F', 'X', '0', '1'};
constexpr std::uint32_t kFooterVersion = 1;
constexpr std::uint64_t kMaxArchiveBytes = 4ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxEntryBytes = 2ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxExpandedBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxEntries = 200000;

#pragma pack(push, 1)
struct Footer {
    char magic[8];
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint64_t offset;
    std::uint64_t size;
};
#pragma pack(pop)
static_assert(sizeof(Footer) == 32, "SFX footer layout changed");

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

std::uint32_t crc_table_value(std::uint32_t index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
        value = (value >> 1u) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return value;
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            values[i] = crc_table_value(i);
        }
        return values;
    }();
    return table;
}

std::uint32_t crc_update(std::uint32_t crc, const std::uint8_t* data,
                         std::size_t size) {
    const auto& table = crc_table();
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8u);
    }
    return crc;
}

bool is_directory_name(const std::string& name) {
    return !name.empty() && (name.back() == '/' || name.back() == '\\');
}

bool safe_relative_name(const std::string& name, std::wstring& relative,
                        DWORD& error, std::wstring& detail) {
    if (name.empty() || name.find('\0') != std::string::npos) {
        error = ERROR_BAD_PATHNAME;
        detail = L"The ZIP contains an empty or NUL-containing file name.";
        return false;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), nullptr, 0);
    if (needed <= 0) {
        error = ERROR_NO_UNICODE_TRANSLATION;
        detail = L"The ZIP contains a file name that is not valid UTF-8.";
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                        static_cast<int>(name.size()), wide.data(), needed);

    std::wstring normalized;
    normalized.reserve(wide.size());
    for (const wchar_t c : wide) {
        normalized.push_back(c == L'/' ? L'\\' : c);
    }
    while (!normalized.empty() &&
           (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }
    if (normalized.empty()) {
        error = ERROR_BAD_PATHNAME;
        detail = L"The ZIP contains an entry with no usable path.";
        return false;
    }
    if (normalized[0] == L'\\' ||
        (normalized.size() >= 2 && normalized[1] == L':')) {
        error = ERROR_BAD_PATHNAME;
        detail = L"The ZIP contains an absolute path.";
        return false;
    }

    static const std::array<const wchar_t*, 22> kReserved{
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
        L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7",
        L"LPT8", L"LPT9"};

    std::wstring rebuilt;
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t slash = normalized.find(L'\\', start);
        std::wstring part = normalized.substr(
            start, slash == std::wstring::npos ? std::wstring::npos
                                               : slash - start);
        if (part.empty() || part == L"." || part == L"..") {
            error = ERROR_BAD_PATHNAME;
            detail = L"The ZIP contains an unsafe path component.";
            return false;
        }
        if (part.find(L':') != std::wstring::npos ||
            part.find_first_of(L"*?\"<>|") != std::wstring::npos) {
            error = ERROR_BAD_PATHNAME;
            detail = L"The ZIP contains a file name Windows cannot create.";
            return false;
        }
        for (const wchar_t c : part) {
            if (c < 32) {
                error = ERROR_BAD_PATHNAME;
                detail = L"The ZIP contains a control character in a path.";
                return false;
            }
        }
        if (part.back() == L'.' || part.back() == L' ') {
            error = ERROR_BAD_PATHNAME;
            detail = L"The ZIP contains a file name ending in a dot or space.";
            return false;
        }
        std::wstring base = part;
        const std::size_t dot = base.find(L'.');
        if (dot != std::wstring::npos) base.resize(dot);
        for (const wchar_t* reserved : kReserved) {
            if (lower_ascii(base) == lower_ascii(reserved)) {
                error = ERROR_BAD_PATHNAME;
                detail = L"The ZIP contains a reserved Windows device name.";
                return false;
            }
        }
        if (!rebuilt.empty()) rebuilt.push_back(L'\\');
        rebuilt += part;
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    if (rebuilt.size() > 30000) {
        error = ERROR_FILENAME_EXCED_RANGE;
        detail = L"The ZIP contains a path that is too long.";
        return false;
    }
    relative = std::move(rebuilt);
    return true;
}

bool write_all(HANDLE file, const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size - written, 1u << 20));
        DWORD actual = 0;
        if (!WriteFile(file, data + written, chunk, &actual, nullptr) ||
            actual == 0) {
            DWORD failure = GetLastError();
            if (failure == ERROR_SUCCESS) failure = ERROR_WRITE_FAULT;
            SetLastError(failure);
            return false;
        }
        written += actual;
    }
    return true;
}

class FileSink final : public InflateSink {
public:
    FileSink(HANDLE file, InstallProgress& progress)
        : file_(file), progress_(progress) {}

    bool write(const std::uint8_t* data, std::size_t size) override {
        if (progress_.cancel.load(std::memory_order_relaxed)) {
            error = ERROR_CANCELLED;
            error_detail = L"Canceled by the user.";
            return false;
        }
        if (!write_all(file_, data, size)) {
            error = GetLastError();
            error_detail = L"Writing the extracted file failed.";
            return false;
        }
        crc = crc_update(crc, data, size);
        written += size;
        progress_.bytes_done.fetch_add(size, std::memory_order_relaxed);
        return true;
    }

    std::uint32_t crc = 0xffffffffu;
    std::uint64_t written = 0;

private:
    HANDLE file_;
    InstallProgress& progress_;
};

class NullSink final : public InflateSink {
public:
    explicit NullSink(InstallProgress* progress) : progress_(progress) {}

    bool write(const std::uint8_t* data, std::size_t size) override {
        if (progress_ &&
            progress_->cancel.load(std::memory_order_relaxed)) {
            error = ERROR_CANCELLED;
            error_detail = L"Canceled by the user.";
            return false;
        }
        crc = crc_update(crc, data, size);
        written += size;
        if (progress_) {
            progress_->bytes_done.fetch_add(size, std::memory_order_relaxed);
        }
        return true;
    }

    std::uint32_t crc = 0xffffffffu;
    std::uint64_t written = 0;

private:
    InstallProgress* progress_ = nullptr;
};

DWORD win32_from_hresult(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return static_cast<DWORD>(HRESULT_CODE(hr));
    }
    return ERROR_FILE_CORRUPT;
}

} // namespace

void InstallProgress::set_current(const std::wstring& file) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = file;
}

std::wstring InstallProgress::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

PayloadArchive::~PayloadArchive() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool PayloadArchive::read_at(std::uint64_t offset, void* data,
                             std::size_t size, DWORD& error,
                             std::wstring& detail) {
    if (offset > payload_size_ || size > payload_size_ - offset) {
        error = ERROR_FILE_CORRUPT;
        detail = L"A ZIP read lies outside the appended payload.";
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(payload_offset_ + offset);
    if (!SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) {
        error = GetLastError();
        detail = L"Seeking inside the setup executable failed.";
        return false;
    }
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size - done, 1u << 20));
        DWORD actual = 0;
        if (!ReadFile(file_, static_cast<unsigned char*>(data) + done, chunk,
                      &actual, nullptr)) {
            error = GetLastError();
            detail = L"Reading the appended payload failed.";
            return false;
        }
        if (actual == 0) {
            error = ERROR_HANDLE_EOF;
            detail = L"Reading the appended payload reached the end early.";
            return false;
        }
        done += actual;
    }
    return true;
}

bool PayloadArchive::open(const std::wstring& exe_path, std::wstring& detail,
                          DWORD& error) {
    error = ERROR_SUCCESS;
    detail.clear();
    entries_.clear();
    total_uncompressed_ = 0;
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    payload_offset_ = 0;
    payload_size_ = 0;

    file_ = CreateFileW(long_path(exe_path).c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        detail = L"The setup executable could not be opened.";
        return false;
    }
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_, &file_size)) {
        error = GetLastError();
        detail = L"The setup executable size could not be read.";
        return false;
    }
    if (file_size.QuadPart < 32) {
        error = ERROR_NOT_FOUND;
        detail = L"This executable has no appended Aurora payload.";
        return false;
    }
    const std::uint64_t total_size =
        static_cast<std::uint64_t>(file_size.QuadPart);
    Footer footer{};
    LARGE_INTEGER footer_pos{};
    footer_pos.QuadPart = static_cast<LONGLONG>(total_size - sizeof(Footer));
    DWORD footer_read = 0;
    if (!SetFilePointerEx(file_, footer_pos, nullptr, FILE_BEGIN) ||
        !ReadFile(file_, &footer, static_cast<DWORD>(sizeof(footer)),
                  &footer_read, nullptr) ||
        footer_read != sizeof(footer)) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_HANDLE_EOF;
        detail = L"The setup footer could not be read.";
        return false;
    }
    if (std::memcmp(footer.magic, kFooterMagic, sizeof(kFooterMagic)) != 0) {
        error = ERROR_NOT_FOUND;
        detail = L"This executable has no appended Aurora payload. Build the "
                 L"installer with packaging/windows/build_installer.sh.";
        return false;
    }
    if (footer.version != kFooterVersion) {
        error = ERROR_NOT_SUPPORTED;
        detail = L"The setup payload uses an unsupported footer version.";
        return false;
    }
    if (footer.offset > total_size ||
        footer.size > total_size - footer.offset ||
        footer.offset + footer.size + sizeof(Footer) != total_size) {
        error = ERROR_FILE_CORRUPT;
        detail = L"The setup footer does not match the executable size.";
        return false;
    }
    payload_offset_ = footer.offset;
    payload_size_ = footer.size;
    if (payload_size_ < 22 || payload_size_ > kMaxArchiveBytes) {
        error = ERROR_FILE_CORRUPT;
        detail = L"The appended payload size is invalid.";
        return false;
    }

    // End of central directory: the packaging step writes no archive comment,
    // but scan the legal comment window so a future packer that adds one still
    // works.
    const std::size_t tail_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(payload_size_, 65557));
    std::vector<unsigned char> tail(tail_size);
    if (!read_at(payload_size_ - tail_size, tail.data(), tail.size(), error,
                 detail)) {
        return false;
    }
    std::size_t eocd = std::numeric_limits<std::size_t>::max();
    for (std::size_t pos = tail_size - 22;; --pos) {
        if (le32(tail.data() + pos) == 0x06054b50u) {
            const std::size_t comment = le16(tail.data() + pos + 20);
            if (pos + 22 + comment == tail_size) {
                eocd = pos;
                break;
            }
        }
        if (pos == 0) break;
    }
    if (eocd == std::numeric_limits<std::size_t>::max()) {
        error = ERROR_FILE_CORRUPT;
        detail = L"The payload ZIP directory is missing.";
        return false;
    }
    if (le16(tail.data() + eocd + 4) != 0 ||
        le16(tail.data() + eocd + 6) != 0) {
        error = ERROR_NOT_SUPPORTED;
        detail = L"Multi-disk ZIP payloads are not supported.";
        return false;
    }
    const std::uint32_t entry_count = le16(tail.data() + eocd + 10);
    const std::uint32_t central_size = le32(tail.data() + eocd + 12);
    const std::uint32_t central_offset = le32(tail.data() + eocd + 16);
    if (entry_count == 0xffffu || central_size == 0xffffffffu ||
        central_offset == 0xffffffffu) {
        error = ERROR_NOT_SUPPORTED;
        detail = L"ZIP64 payloads are not supported.";
        return false;
    }
    if (entry_count == 0 || entry_count > kMaxEntries) {
        error = ERROR_FILE_CORRUPT;
        detail = L"The payload contains an invalid number of files.";
        return false;
    }
    const std::uint64_t eocd_offset =
        payload_size_ - tail_size + static_cast<std::uint64_t>(eocd);
    if (static_cast<std::uint64_t>(central_offset) + central_size >
        eocd_offset) {
        error = ERROR_FILE_CORRUPT;
        detail = L"The ZIP central directory lies outside the payload.";
        return false;
    }
    std::vector<unsigned char> central(central_size);
    if (!read_at(central_offset, central.data(), central.size(), error,
                 detail)) {
        return false;
    }

    std::set<std::wstring> seen;
    std::size_t pos = 0;
    for (std::uint32_t index = 0; index < entry_count; ++index) {
        if (pos + 46 > central.size() ||
            le32(central.data() + pos) != 0x02014b50u) {
            error = ERROR_FILE_CORRUPT;
            detail = L"The ZIP central directory is truncated.";
            return false;
        }
        const std::uint16_t flags = le16(central.data() + pos + 8);
        const std::uint16_t method = le16(central.data() + pos + 10);
        const std::uint32_t crc = le32(central.data() + pos + 16);
        const std::uint32_t compressed = le32(central.data() + pos + 20);
        const std::uint32_t expanded = le32(central.data() + pos + 24);
        const std::uint16_t name_length = le16(central.data() + pos + 28);
        const std::uint16_t extra_length = le16(central.data() + pos + 30);
        const std::uint16_t comment_length = le16(central.data() + pos + 32);
        const std::uint32_t external_attributes =
            le32(central.data() + pos + 38);
        const std::uint32_t local_offset = le32(central.data() + pos + 42);
        const std::size_t next = pos + 46u + name_length + extra_length +
                                 comment_length;
        if (next > central.size() || name_length == 0) {
            error = ERROR_FILE_CORRUPT;
            detail = L"A ZIP directory entry is truncated.";
            return false;
        }
        if ((flags & 1u) != 0) {
            error = ERROR_NOT_SUPPORTED;
            detail = L"Encrypted ZIP entries are not supported.";
            return false;
        }
        if (method != 0 && method != 8) {
            error = ERROR_NOT_SUPPORTED;
            detail = L"The payload uses an unsupported ZIP compression method.";
            return false;
        }
        if (compressed == 0xffffffffu || expanded == 0xffffffffu ||
            local_offset == 0xffffffffu) {
            error = ERROR_NOT_SUPPORTED;
            detail = L"ZIP64 entries are not supported.";
            return false;
        }
        const std::uint32_t unix_mode = (external_attributes >> 16u) & 0170000u;
        if (unix_mode == 0120000u) {
            error = ERROR_NOT_SUPPORTED;
            detail = L"The payload contains a symbolic link.";
            return false;
        }
        if (expanded > kMaxEntryBytes ||
            total_uncompressed_ + expanded > kMaxExpandedBytes) {
            error = ERROR_FILE_TOO_LARGE;
            detail = L"The payload expands beyond the installer's safety limit.";
            return false;
        }

        ZipEntry entry;
        entry.name.assign(
            reinterpret_cast<const char*>(central.data() + pos + 46),
            name_length);
        entry.method = method;
        entry.crc = crc;
        entry.compressed = compressed;
        entry.expanded = expanded;
        entry.local_offset = local_offset;
        entry.directory = is_directory_name(entry.name) ||
                          (external_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!safe_relative_name(entry.name, entry.relative, error, detail)) {
            return false;
        }
        if (!seen.insert(lower_ascii(entry.relative)).second) {
            error = ERROR_FILE_EXISTS;
            detail = L"The payload contains the same path twice.";
            return false;
        }
        total_uncompressed_ += expanded;
        entries_.push_back(std::move(entry));
        pos = next;
    }

    bool browser = false;
    bool bootstrap = false;
    bool manifest = false;
    for (const ZipEntry& entry : entries_) {
        const std::wstring lower = lower_ascii(entry.relative);
        browser = browser ||
                  (!entry.directory && lower == L"aurora_browser.exe");
        bootstrap = bootstrap ||
                    (!entry.directory && lower == L"aurora_bootstrap.exe");
        manifest = manifest ||
                   (!entry.directory && lower == L"aurora_build.manifest");
    }
    if (!browser || !bootstrap || !manifest) {
        error = ERROR_FILE_NOT_FOUND;
        detail = L"The payload is missing aurora_browser.exe, "
                 L"aurora_bootstrap.exe or aurora_build.manifest.";
        return false;
    }
    return true;
}

bool PayloadArchive::extract_entry(const ZipEntry& entry,
                                   const std::wstring& destination,
                                   InstallProgress& progress,
                                   std::wstring& detail, DWORD& error) {
    const std::wstring target = join_path(destination, entry.relative);
    progress.set_current(entry.relative);
    if (entry.directory) {
        if (!ensure_directory(target, error)) {
            detail = L"Creating a directory failed: " + target;
            return false;
        }
        return true;
    }
    const std::wstring parent = parent_directory(target);
    if (!parent.empty() && !ensure_directory(parent, error)) {
        detail = L"Creating a directory failed: " + parent;
        return false;
    }
    if (progress.cancel.load(std::memory_order_relaxed)) {
        error = ERROR_CANCELLED;
        detail = L"Canceled by the user.";
        return false;
    }

    // The destination is a fresh staging directory, so CREATE_ALWAYS only
    // replaces a file the same archive already wrote (a rejected duplicate) or
    // one left by an interrupted previous attempt.
    HANDLE output = CreateFileW(
        long_path(target).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        detail = L"Creating an extracted file failed: " + target;
        return false;
    }

    const std::uint64_t data_offset =
        static_cast<std::uint64_t>(entry.local_offset);
    unsigned char local_header[30]{};
    if (!read_at(data_offset, local_header, sizeof(local_header), error,
                 detail)) {
        CloseHandle(output);
        return false;
    }
    if (le32(local_header) != 0x04034b50u) {
        CloseHandle(output);
        error = ERROR_FILE_CORRUPT;
        detail = L"A ZIP local header is missing: " + entry.relative;
        return false;
    }
    const std::size_t local_name = le16(local_header + 26);
    const std::size_t local_extra = le16(local_header + 28);
    const std::uint64_t content_offset =
        data_offset + 30u + local_name + local_extra;
    if (content_offset > payload_size_ ||
        entry.compressed > payload_size_ - content_offset) {
        CloseHandle(output);
        error = ERROR_FILE_CORRUPT;
        detail = L"A ZIP entry points outside the payload: " + entry.relative;
        return false;
    }

    bool ok = true;
    std::uint32_t crc = 0xffffffffu;
    std::uint64_t written = 0;
    if (entry.method == 0) {
        std::vector<std::uint8_t> buffer(1u << 20);
        std::uint32_t remaining = entry.compressed;
        std::uint64_t position = content_offset;
        while (remaining > 0) {
            if (progress.cancel.load(std::memory_order_relaxed)) {
                error = ERROR_CANCELLED;
                detail = L"Canceled by the user.";
                ok = false;
                break;
            }
            const std::size_t chunk =
                std::min<std::size_t>(remaining, buffer.size());
            if (!read_at(position, buffer.data(), chunk, error, detail)) {
                ok = false;
                break;
            }
            if (!write_all(output, buffer.data(), chunk)) {
                error = GetLastError();
                detail = L"Writing an extracted file failed: " + target;
                ok = false;
                break;
            }
            crc = crc_update(crc, buffer.data(), chunk);
            written += chunk;
            progress.bytes_done.fetch_add(chunk, std::memory_order_relaxed);
            position += chunk;
            remaining -= static_cast<std::uint32_t>(chunk);
        }
    } else {
        if (entry.compressed > 512ull * 1024ull * 1024ull) {
            error = ERROR_FILE_TOO_LARGE;
            detail = L"A compressed entry is too large to decode: " +
                     entry.relative;
            ok = false;
        } else {
            std::vector<std::uint8_t> compressed(entry.compressed);
            if (!read_at(content_offset, compressed.data(), compressed.size(),
                         error, detail)) {
                ok = false;
            } else {
                FileSink sink(output, progress);
                InflateResult inflated = inflate_raw_stream(
                    compressed.data(), compressed.size(), entry.expanded, sink);
                if (!inflated.ok) {
                    error = inflated.error == S_OK
                                ? ERROR_FILE_CORRUPT
                                : win32_from_hresult(inflated.error);
                    detail = L"Decompressing " + entry.relative + L" failed: " +
                             inflated.detail;
                    ok = false;
                } else {
                    crc = sink.crc;
                    written = sink.written;
                }
            }
        }
    }

    CloseHandle(output);
    if (!ok) {
        DeleteFileW(long_path(target).c_str());
        return false;
    }
    if (written != entry.expanded ||
        (crc ^ 0xffffffffu) != entry.crc) {
        DeleteFileW(long_path(target).c_str());
        error = ERROR_CRC;
        detail = L"CRC or size mismatch for " + entry.relative;
        return false;
    }
    return true;
}

bool PayloadArchive::extract(const std::wstring& destination,
                             InstallProgress& progress,
                             std::wstring& failed_path, std::wstring& detail,
                             DWORD& error) {
    error = ERROR_SUCCESS;
    detail.clear();
    failed_path.clear();
    if (!is_open()) {
        error = ERROR_NOT_FOUND;
        detail = L"The payload is not open.";
        return false;
    }
    progress.bytes_total.store(total_uncompressed_, std::memory_order_relaxed);
    progress.bytes_done.store(0, std::memory_order_relaxed);
    if (!ensure_directory(destination, error)) {
        failed_path = destination;
        detail = L"Creating the staging folder failed: " + destination;
        return false;
    }
    for (const ZipEntry& entry : entries_) {
        if (progress.cancel.load(std::memory_order_relaxed)) {
            error = ERROR_CANCELLED;
            detail = L"Canceled by the user.";
            return false;
        }
        if (!extract_entry(entry, destination, progress, detail, error)) {
            failed_path = join_path(destination, entry.relative);
            return false;
        }
    }
    return true;
}

bool PayloadArchive::verify_entry(const ZipEntry& entry, std::wstring& detail,
                                  DWORD& error) {
    if (entry.directory) return true;
    const std::uint64_t data_offset =
        static_cast<std::uint64_t>(entry.local_offset);
    unsigned char local_header[30]{};
    if (!read_at(data_offset, local_header, sizeof(local_header), error,
                 detail)) {
        return false;
    }
    if (le32(local_header) != 0x04034b50u) {
        error = ERROR_FILE_CORRUPT;
        detail = L"A ZIP local header is missing: " + entry.relative;
        return false;
    }
    const std::size_t local_name = le16(local_header + 26);
    const std::size_t local_extra = le16(local_header + 28);
    const std::uint64_t content_offset =
        data_offset + 30u + local_name + local_extra;
    if (content_offset > payload_size_ ||
        entry.compressed > payload_size_ - content_offset) {
        error = ERROR_FILE_CORRUPT;
        detail = L"A ZIP entry points outside the payload: " + entry.relative;
        return false;
    }
    if (entry.method == 0) {
        std::vector<std::uint8_t> buffer(1u << 20);
        std::uint32_t remaining = entry.compressed;
        std::uint64_t position = content_offset;
        std::uint32_t crc = 0xffffffffu;
        std::uint64_t written = 0;
        while (remaining > 0) {
            const std::size_t chunk =
                std::min<std::size_t>(remaining, buffer.size());
            if (!read_at(position, buffer.data(), chunk, error, detail)) {
                return false;
            }
            crc = crc_update(crc, buffer.data(), chunk);
            written += chunk;
            position += chunk;
            remaining -= static_cast<std::uint32_t>(chunk);
        }
        if (written != entry.expanded ||
            (crc ^ 0xffffffffu) != entry.crc) {
            error = ERROR_CRC;
            detail = L"CRC or size mismatch for " + entry.relative;
            return false;
        }
        return true;
    }
    if (entry.compressed > 512ull * 1024ull * 1024ull) {
        error = ERROR_FILE_TOO_LARGE;
        detail = L"A compressed entry is too large to decode: " + entry.relative;
        return false;
    }
    std::vector<std::uint8_t> compressed(entry.compressed);
    if (!read_at(content_offset, compressed.data(), compressed.size(), error,
                 detail)) {
        return false;
    }
    NullSink sink(nullptr);
    InflateResult inflated = inflate_raw_stream(
        compressed.data(), compressed.size(), entry.expanded, sink);
    if (!inflated.ok) {
        error = inflated.error == S_OK ? ERROR_FILE_CORRUPT
                                       : win32_from_hresult(inflated.error);
        detail = L"Decompressing " + entry.relative + L" failed: " +
                 inflated.detail;
        return false;
    }
    if (sink.written != entry.expanded ||
        (sink.crc ^ 0xffffffffu) != entry.crc) {
        error = ERROR_CRC;
        detail = L"CRC or size mismatch for " + entry.relative;
        return false;
    }
    return true;
}

bool PayloadArchive::verify(std::wstring& detail, DWORD& error,
                            std::uint32_t& files, std::uint64_t& bytes) {
    error = ERROR_SUCCESS;
    detail.clear();
    files = 0;
    bytes = 0;
    if (!is_open()) {
        error = ERROR_NOT_FOUND;
        detail = L"The payload is not open.";
        return false;
    }
    for (const ZipEntry& entry : entries_) {
        if (entry.directory) continue;
        if (!verify_entry(entry, detail, error)) return false;
        ++files;
        bytes += entry.expanded;
    }
    return true;
}

} // namespace aurora::setup
