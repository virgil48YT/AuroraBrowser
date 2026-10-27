// Aurora Browser Setup — payload reader and ZIP extractor.
//
// The setup .exe is a small native UI with a ZIP payload appended after it (see
// packaging/windows/make_sfx.py). A 32-byte footer at the very end points at
// the ZIP, which lets the installer read its own payload without copying a
// temporary archive first. Extraction validates every entry, streams progress
// into the UI and reports the real Win32 error for the operation that failed.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aurora::setup {

struct InstallProgress {
    std::atomic<std::uint64_t> bytes_done{0};
    std::atomic<std::uint64_t> bytes_total{0};
    std::atomic<bool> cancel{false};

    void set_current(const std::wstring& file);
    std::wstring current() const;

private:
    mutable std::mutex mutex_;
    std::wstring current_;
};

struct ZipEntry {
    std::string name;         // UTF-8 name from the ZIP directory
    std::wstring relative;    // validated Windows relative path (backslashes)
    bool directory = false;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed = 0;
    std::uint32_t expanded = 0;
    std::uint32_t local_offset = 0;
};

class PayloadArchive {
public:
    PayloadArchive() = default;
    ~PayloadArchive();
    PayloadArchive(const PayloadArchive&) = delete;
    PayloadArchive& operator=(const PayloadArchive&) = delete;

    // Opens this executable (or another setup .exe), locates the appended ZIP
    // and parses its central directory. `detail` is for the log; `error` is a
    // real Win32 error code suitable for HRESULT_FROM_WIN32 / FormatMessage.
    bool open(const std::wstring& exe_path, std::wstring& detail, DWORD& error);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    std::uint64_t total_uncompressed() const { return total_uncompressed_; }
    std::uint64_t payload_size() const { return payload_size_; }
    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }

    // Extracts into an already-created (or creatable) destination. On failure
    // `failed_path` names the file that failed and `error` is the Win32 error
    // from the failing operation (or a ZIP integrity error).
    bool extract(const std::wstring& destination, InstallProgress& progress,
                 std::wstring& failed_path, std::wstring& detail, DWORD& error);

    // Decompresses every file to a null sink and verifies sizes and CRCs.
    // Used by --verify-payload in CI; writes nothing.
    bool verify(std::wstring& detail, DWORD& error, std::uint32_t& files,
                std::uint64_t& bytes);

private:
    bool read_at(std::uint64_t offset, void* data, std::size_t size,
                 DWORD& error, std::wstring& detail);
    bool extract_entry(const ZipEntry& entry, const std::wstring& destination,
                       InstallProgress& progress, std::wstring& detail,
                       DWORD& error);
    bool verify_entry(const ZipEntry& entry, std::wstring& detail,
                      DWORD& error);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::uint64_t payload_offset_ = 0;
    std::uint64_t payload_size_ = 0;
    std::vector<ZipEntry> entries_;
    std::uint64_t total_uncompressed_ = 0;
};

} // namespace aurora::setup
