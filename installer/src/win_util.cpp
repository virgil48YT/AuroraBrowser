#include "win_util.hpp"

#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace aurora::setup {
namespace {

std::wstring g_log_path;
std::mutex g_log_mutex;

std::wstring default_log_path() {
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length > MAX_PATH) return L"aurora-setup.log";
    std::wstring path(temp, length);
    if (path.empty()) path = L".";
    if (path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
    return path + L"aurora-setup.log";
}

std::wstring timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, st.wMilliseconds);
    return buffer;
}

bool is_separator(wchar_t c) { return c == L'\\' || c == L'/'; }

} // namespace

std::wstring module_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) return {};
    }
}

std::wstring exe_directory() {
    const std::wstring path = module_path();
    return path.empty() ? std::wstring() : parent_directory(path);
}

std::wstring join_path(std::wstring_view base, std::wstring_view leaf) {
    if (base.empty()) return std::wstring(leaf);
    std::wstring path(base);
    if (!path.empty() && !is_separator(path.back())) path.push_back(L'\\');
    path.append(leaf);
    return path;
}

std::wstring normalize_path(const std::wstring& path) {
    if (path.empty()) return {};
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;
    std::wstring full(needed, L'\0');
    const DWORD written =
        GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) return path;
    full.resize(written);
    while (full.size() > 3 && is_separator(full.back())) full.pop_back();
    return full;
}

std::wstring long_path(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    if (path.size() >= 2 && path[1] == L':') return L"\\\\?\\" + path;
    return path;
}

std::wstring parent_directory(const std::wstring& path) {
    if (path.empty()) return {};
    size_t end = path.size();
    while (end > 1 && is_separator(path[end - 1])) --end;
    if (end == 3 && path[1] == L':') return {};
    size_t pos = end;
    while (pos > 0 && !is_separator(path[pos - 1])) --pos;
    if (pos == 0) return {};
    while (pos > 1 && is_separator(path[pos - 1])) --pos;
    if (pos == 3 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, pos);
}

std::wstring file_name(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring trim_copy(const std::wstring& text) {
    size_t begin = 0;
    while (begin < text.size() && iswspace(text[begin])) ++begin;
    size_t end = text.size();
    while (end > begin && iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring lower_ascii(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return text;
}

bool is_absolute_path(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    return path.size() >= 2 && is_separator(path[0]) && is_separator(path[1]);
}

bool is_safe_install_dir(const std::wstring& path, std::wstring& why) {
    if (path.empty()) {
        why = L"The install folder is empty.";
        return false;
    }
    if (!is_absolute_path(path)) {
        why = L"The install folder must be a full path.";
        return false;
    }
    std::wstring clean = normalize_path(path);
    if (clean.size() <= 3) {
        why = L"Choose a folder, not a drive root.";
        return false;
    }
    const std::wstring lower = lower_ascii(clean);
    if (lower == L"c:\\windows" || lower == L"c:\\program files" ||
        lower == L"c:\\program files (x86)" || lower == L"c:\\programdata") {
        why = L"Choose a subfolder for Aurora, not this system folder.";
        return false;
    }
    return true;
}

bool path_is_under(const std::wstring& path, const std::wstring& directory) {
    if (path.empty() || directory.empty()) return false;
    std::wstring a = lower_ascii(normalize_path(path));
    std::wstring b = lower_ascii(normalize_path(directory));
    while (b.size() > 3 && is_separator(b.back())) b.pop_back();
    if (a.size() <= b.size()) return false;
    if (a.compare(0, b.size(), b) != 0) return false;
    return is_separator(a[b.size()]);
}

bool file_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(long_path(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool directory_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(long_path(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ensure_directory(const std::wstring& path, DWORD& error) {
    error = ERROR_SUCCESS;
    if (path.empty()) {
        error = ERROR_PATH_NOT_FOUND;
        return false;
    }
    if (directory_exists(path)) return true;
    const std::wstring parent = parent_directory(path);
    if (!parent.empty() && parent.size() < path.size() &&
        !ensure_directory(parent, error)) {
        return false;
    }
    if (CreateDirectoryW(long_path(path).c_str(), nullptr)) return true;
    error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS && directory_exists(path)) {
        error = ERROR_SUCCESS;
        return true;
    }
    return false;
}

uint64_t file_size_win32(const std::wstring& path, DWORD& error) {
    error = ERROR_SUCCESS;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(long_path(path).c_str(), GetFileExInfoStandard,
                              &data)) {
        error = GetLastError();
        return 0;
    }
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart;
}

uint64_t directory_size(const std::wstring& path) {
    if (path.empty()) return 0;
    std::wstring spec = long_path(path);
    if (!spec.empty() && !is_separator(spec.back())) spec.push_back(L'\\');
    spec += L'*';
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(spec.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    uint64_t total = 0;
    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring child = join_path(path, data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                total += directory_size(child);
            }
        } else {
            ULARGE_INTEGER size{};
            size.HighPart = data.nFileSizeHigh;
            size.LowPart = data.nFileSizeLow;
            total += size.QuadPart;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return total;
}

bool delete_tree(const std::wstring& path, std::wstring* first_error) {
    if (path.empty()) return true;
    const DWORD attrs = GetFileAttributesW(long_path(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return true;
        }
    }
    std::wstring spec = long_path(path);
    if (!spec.empty() && !is_separator(spec.back())) spec.push_back(L'\\');
    spec += L'*';
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(spec.c_str(), &data);
    bool ok = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring child = join_path(path, data.cFileName);
            const bool is_dir =
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_link =
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (is_dir && !is_link) {
                if (!delete_tree(child, first_error)) ok = false;
            } else {
                if (!DeleteFileW(long_path(child).c_str())) {
                    const DWORD error = GetLastError();
                    SetFileAttributesW(long_path(child).c_str(),
                                       FILE_ATTRIBUTE_NORMAL);
                    if (!DeleteFileW(long_path(child).c_str())) {
                        ok = false;
                        if (first_error && first_error->empty()) {
                            *first_error = L"DeleteFile " + child + L" (" +
                                           format_win32_message(error) + L")";
                        }
                    }
                }
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    if (!RemoveDirectoryW(long_path(path).c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            ok = false;
            if (first_error && first_error->empty()) {
                *first_error = L"RemoveDirectory " + path + L" (" +
                               format_win32_message(error) + L")";
            }
        }
    }
    return ok;
}

std::wstring format_win32_message(DWORD error) {
    if (error == ERROR_SUCCESS) return L"The operation completed successfully.";
    LPWSTR text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring message;
    if (length > 0 && text) message.assign(text, length);
    if (text) LocalFree(text);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
        message.pop_back();
    }
    if (message.empty()) message = L"Unknown error.";
    return message;
}

std::wstring format_hresult(HRESULT hr) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08lX",
               static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

std::wstring format_bytes(uint64_t bytes) {
    constexpr uint64_t kKiB = 1024;
    constexpr uint64_t kMiB = 1024 * 1024;
    constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    if (bytes >= kGiB) {
        const uint64_t tenths = (bytes * 10 + kGiB / 2) / kGiB;
        wchar_t buffer[32]{};
        if (tenths % 10 == 0) {
            swprintf_s(buffer, L"%llu GB",
                       static_cast<unsigned long long>(tenths / 10));
        } else {
            swprintf_s(buffer, L"%llu.%llu GB",
                       static_cast<unsigned long long>(tenths / 10),
                       static_cast<unsigned long long>(tenths % 10));
        }
        return buffer;
    }
    if (bytes >= kMiB) {
        const uint64_t mib = (bytes + kMiB - 1) / kMiB;
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%llu MB",
                   static_cast<unsigned long long>(mib));
        return buffer;
    }
    const uint64_t kib = (bytes + kKiB - 1) / kKiB;
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%llu KB", static_cast<unsigned long long>(kib));
    return buffer;
}

std::wstring get_known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) ||
        path == nullptr) {
        if (path) CoTaskMemFree(path);
        return {};
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

std::wstring roaming_aurora_dir() {
    std::wstring appdata = get_known_folder(FOLDERID_RoamingAppData);
    if (appdata.empty()) {
        wchar_t buffer[MAX_PATH + 1]{};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer,
                                                     MAX_PATH);
        if (length > 0) appdata.assign(buffer, length);
    }
    return appdata.empty() ? std::wstring() : join_path(appdata, L"Aurora");
}

std::wstring quote_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) return arg;
    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring command_line_for(const std::wstring& exe,
                              const std::vector<std::wstring>& args) {
    std::wstring line = quote_arg(exe);
    for (const std::wstring& arg : args) {
        line.push_back(L' ');
        line += quote_arg(arg);
    }
    return line;
}

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

void set_log_path(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_path = path.empty() ? default_log_path() : path;
}

const std::wstring& log_path() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_path.empty()) g_log_path = default_log_path();
    return g_log_path;
}

void log_line(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_path.empty()) g_log_path = default_log_path();
    const std::wstring directory = parent_directory(g_log_path);
    if (!directory.empty()) {
        DWORD ignored = ERROR_SUCCESS;
        ensure_directory(directory, ignored);
    }
    HANDLE file = CreateFileW(long_path(g_log_path).c_str(),
                              FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const std::wstring entry =
        timestamp() + L"  " + line + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, entry.c_str(),
                                          static_cast<int>(entry.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, entry.c_str(),
                            static_cast<int>(entry.size()), utf8.data(), bytes,
                            nullptr, nullptr);
        if (size.QuadPart == 0) {
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            DWORD written = 0;
            WriteFile(file, bom, 3, &written, nullptr);
        }
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                  nullptr);
    }
    CloseHandle(file);
}

} // namespace aurora::setup
