#include "install.hpp"

#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <tlhelp32.h>

#include "win_util.hpp"

namespace aurora::setup {
namespace {

constexpr std::uint64_t kFreeSpaceMargin = 64ull * 1024ull * 1024ull;

std::wstring current_timestamp_for_dir() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    return buffer;
}

std::wstring staging_directory(const std::wstring& parent) {
    const DWORD pid = GetCurrentProcessId();
    const ULONGLONG tick = GetTickCount64();
    wchar_t suffix[64]{};
    swprintf_s(suffix, L".aurora-setup-%lu-%llu",
               static_cast<unsigned long>(pid),
               static_cast<unsigned long long>(tick));
    return join_path(parent, suffix);
}

bool write_utf8_file(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(long_path(path).c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(),
                              static_cast<DWORD>(bytes.size()), &written,
                              nullptr) != FALSE &&
                    static_cast<std::size_t>(written) == bytes.size();
    CloseHandle(file);
    return ok;
}

bool read_registry_string(const wchar_t* name, std::wstring& value) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AuroraBrowser", name,
                     RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return false;
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AuroraBrowser", name,
                     RRF_RT_REG_SZ, &type, buffer.data(), &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    value = std::move(buffer);
    return !value.empty();
}

void set_registry_string(HKEY key, const wchar_t* name,
                         const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

} // namespace

std::wstring language_tag(Language language) {
    const LanguageInfo* list = language_list();
    const int index = language_index(language);
    return (index >= 0 && index < language_count()) ? list[index].tag
                                                    : L"en-US";
}

std::wstring default_install_dir() {
    std::wstring registered;
    if (read_registry_string(L"InstallDir", registered) &&
        is_absolute_path(registered)) {
        return registered;
    }
    // Upgrades from the old 7-Zip SFX installed under LOCALAPPDATA. Prefer
    // that tree instead of leaving a second, parallel copy in Program Files.
    std::wstring local = get_known_folder(FOLDERID_LocalAppData);
    if (!local.empty()) {
        const std::wstring legacy =
            join_path(join_path(local, L"Aurora"), L"app");
        if (file_exists(join_path(legacy, L"aurora_browser.exe"))) {
            return legacy;
        }
    }
    std::wstring program_files = get_known_folder(FOLDERID_ProgramFiles);
    if (program_files.empty()) {
        wchar_t buffer[MAX_PATH + 1]{};
        const DWORD length =
            GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH);
        if (length > 0) program_files.assign(buffer, length);
    }
    if (program_files.empty()) program_files = L"C:\\Program Files";
    return join_path(program_files, L"Aurora Browser");
}

uint64_t free_bytes_for_path(const std::wstring& path) {
    std::wstring probe = normalize_path(path);
    while (!probe.empty() && !directory_exists(probe) &&
           probe.size() > 3) {
        const std::wstring parent = parent_directory(probe);
        if (parent.empty() || parent == probe) break;
        probe = parent;
    }
    wchar_t volume[MAX_PATH + 1]{};
    if (!GetVolumePathNameW(probe.c_str(), volume, MAX_PATH)) {
        if (probe.size() >= 3 && probe[1] == L':') {
            volume[0] = probe[0];
            volume[1] = L':';
            volume[2] = L'\\';
            volume[3] = L'\0';
        } else {
            return 0;
        }
    }
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(volume, &free_bytes, nullptr, nullptr)) {
        return 0;
    }
    return free_bytes.QuadPart;
}

std::vector<std::wstring> running_aurora_processes(
    const std::wstring& install_dir) {
    std::vector<std::wstring> result;
    if (install_dir.empty()) return result;
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self || entry.th32ProcessID == 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t image[32768]{};
            DWORD size = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
            if (QueryFullProcessImageNameW(process, 0, image, &size) &&
                path_is_under(image, install_dir)) {
                wchar_t line[512]{};
                swprintf_s(line, L"%s (PID %lu)", entry.szExeFile,
                           static_cast<unsigned long>(entry.th32ProcessID));
                result.emplace_back(line);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool wait_for_aurora_exit(const std::wstring& install_dir, int seconds) {
    const ULONGLONG deadline =
        GetTickCount64() + static_cast<ULONGLONG>(std::max(seconds, 0)) * 1000;
    for (;;) {
        if (running_aurora_processes(install_dir).empty()) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(250);
    }
}

bool shortcut_exists_for(bool desktop) {
    const KNOWNFOLDERID id = desktop ? FOLDERID_Desktop : FOLDERID_Programs;
    const std::wstring folder = get_known_folder(id);
    if (folder.empty()) return false;
    return file_exists(join_path(folder, L"Aurora Browser.lnk"));
}

bool create_shortcut(bool desktop, const std::wstring& install_dir,
                     std::wstring& detail, HRESULT& error) {
    detail.clear();
    error = S_OK;
    const KNOWNFOLDERID id = desktop ? FOLDERID_Desktop : FOLDERID_Programs;
    const std::wstring folder = get_known_folder(id);
    if (folder.empty()) {
        error = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        detail = L"The shell folder for the shortcut is unavailable.";
        return false;
    }
    const std::wstring link_path =
        join_path(folder, L"Aurora Browser.lnk");
    const std::wstring target =
        join_path(install_dir, L"aurora_bootstrap.exe");
    const std::wstring icon =
        join_path(install_dir, L"aurora_browser.exe");

    IShellLinkW* shell_link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&shell_link));
    if (FAILED(hr) || !shell_link) {
        error = hr;
        detail = L"CoCreateInstance(CLSID_ShellLink) failed.";
        return false;
    }
    shell_link->SetPath(target.c_str());
    shell_link->SetWorkingDirectory(install_dir.c_str());
    shell_link->SetDescription(L"Aurora Browser");
    shell_link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* persist = nullptr;
    hr = shell_link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr) && persist) {
        hr = persist->Save(link_path.c_str(), TRUE);
        persist->Release();
    }
    shell_link->Release();
    if (FAILED(hr)) {
        error = hr;
        detail = L"Writing the shortcut failed: " + link_path;
        return false;
    }
    return true;
}

bool launch_aurora(const std::wstring& install_dir,
                   const std::wstring& language, bool pass_language,
                   std::wstring& detail, DWORD& error) {
    detail.clear();
    error = ERROR_SUCCESS;
    const std::wstring exe = join_path(install_dir, L"aurora_bootstrap.exe");
    std::wstring command = command_line_for(
        exe, pass_language && !language.empty()
                 ? std::vector<std::wstring>{L"--aurora-language=" + language}
                 : std::vector<std::wstring>{});
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, install_dir.c_str(), &startup,
                        &process)) {
        error = GetLastError();
        detail = L"CreateProcessW failed for " + exe;
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool relaunch_elevated(const std::vector<std::wstring>& args,
                       std::wstring& detail, DWORD& error) {
    detail.clear();
    error = ERROR_SUCCESS;
    std::wstring parameters;
    for (const std::wstring& arg : args) {
        if (!parameters.empty()) parameters.push_back(L' ');
        parameters += quote_arg(arg);
    }
    const std::wstring exe = module_path();
    const std::wstring directory = exe_directory();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        error = GetLastError();
        detail = L"Elevation was canceled or could not be started.";
        return false;
    }
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

void write_registry(const InstallOptions& options) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AuroraBrowser", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        log_line(L"Registry: could not open HKCU\\Software\\AuroraBrowser");
        return;
    }
    set_registry_string(key, L"InstallDir", options.install_dir);
    set_registry_string(key, L"Version", options.version);
    set_registry_string(key, L"Language", language_tag(options.language));
    set_registry_string(key, L"InstallDate", current_timestamp_for_dir());
    RegCloseKey(key);
}

void write_language_marker(Language language) {
    const std::wstring dir = roaming_aurora_dir();
    if (dir.empty()) return;
    DWORD error = ERROR_SUCCESS;
    if (!ensure_directory(dir, error)) {
        log_line(L"Language marker: cannot create " + dir + L" (" +
                 format_win32_message(error) + L")");
        return;
    }
    const std::string utf8 = [&] {
        const std::wstring tag = language_tag(language);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, tag.c_str(),
                                              static_cast<int>(tag.size()),
                                              nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
        if (bytes > 0) {
            WideCharToMultiByte(CP_UTF8, 0, tag.c_str(),
                                static_cast<int>(tag.size()), text.data(),
                                bytes, nullptr, nullptr);
        }
        return text;
    }();
    if (!write_utf8_file(join_path(dir, L"install-language.txt"), utf8)) {
        log_line(L"Language marker: could not write install-language.txt");
    }
}

InstallResult install_from_payload(PayloadArchive& archive,
                                   const InstallOptions& options,
                                   InstallProgress& progress) {
    InstallResult result;
    result.install_dir = normalize_path(trim_copy(options.install_dir));
    result.detail = L"";
    result.error = ERROR_SUCCESS;
    result.hr = S_OK;

    std::wstring why;
    if (!is_safe_install_dir(result.install_dir, why)) {
        result.error = ERROR_BAD_PATHNAME;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = why;
        return result;
    }
    const std::wstring parent = parent_directory(result.install_dir);
    if (parent.empty()) {
        result.error = ERROR_PATH_NOT_FOUND;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = L"The install folder has no parent directory.";
        return result;
    }
    DWORD fs_error = ERROR_SUCCESS;
    if (!ensure_directory(parent, fs_error)) {
        result.error = fs_error;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = L"Creating the install parent folder failed: " + parent;
        return result;
    }
    if (file_exists(result.install_dir) &&
        !directory_exists(result.install_dir)) {
        result.error = ERROR_FILE_EXISTS;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = L"A file already exists at the install path.";
        return result;
    }
    if (directory_exists(result.install_dir)) {
        const bool looks_like_aurora =
            file_exists(join_path(result.install_dir, L"aurora_browser.exe")) ||
            file_exists(join_path(result.install_dir, L"aurora_build.manifest"));
        if (!looks_like_aurora &&
            !PathIsDirectoryEmptyW(
                long_path(result.install_dir).c_str())) {
            result.error = ERROR_DIR_NOT_EMPTY;
            result.hr = HRESULT_FROM_WIN32(result.error);
            result.failed_path = result.install_dir;
            result.detail =
                L"The chosen folder is not empty and does not contain Aurora.";
            return result;
        }
    }

    const std::uint64_t required =
        archive.total_uncompressed() + kFreeSpaceMargin;
    const std::uint64_t free_bytes =
        free_bytes_for_path(result.install_dir);
    result.fresh_install = !directory_exists(result.install_dir);
    if (free_bytes > 0 && free_bytes < required) {
        result.error = ERROR_DISK_FULL;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = L"Not enough free space: need " +
                        format_bytes(required) + L", have " +
                        format_bytes(free_bytes) + L".";
        return result;
    }

    const std::wstring staging = staging_directory(parent);
    if (!ensure_directory(staging, fs_error)) {
        result.error = fs_error;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.failed_path = staging;
        result.detail = L"Creating the staging folder failed: " + staging;
        return result;
    }
    log_line(L"Staging extraction: " + staging);

    std::wstring extract_detail;
    std::wstring extract_failed;
    DWORD extract_error = ERROR_SUCCESS;
    if (!archive.extract(staging, progress, extract_failed, extract_detail,
                         extract_error)) {
        delete_tree(staging, nullptr);
        result.error = extract_error;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = extract_detail;
        const std::wstring staging_prefix = staging + L"\\";
        result.failed_path =
            extract_failed.rfind(staging_prefix, 0) == 0
                ? join_path(result.install_dir,
                            extract_failed.substr(staging_prefix.size()))
                : extract_failed;
        result.canceled = extract_error == ERROR_CANCELLED ||
                          progress.cancel.load(std::memory_order_relaxed);
        return result;
    }
    if (progress.cancel.load(std::memory_order_relaxed) ||
        extract_error == ERROR_CANCELLED) {
        delete_tree(staging, nullptr);
        result.canceled = true;
        result.error = ERROR_CANCELLED;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = L"Canceled by the user.";
        return result;
    }

    for (const wchar_t* required_file :
         {L"aurora_browser.exe", L"aurora_bootstrap.exe",
          L"aurora_build.manifest"}) {
        if (!file_exists(join_path(staging, required_file))) {
            delete_tree(staging, nullptr);
            result.error = ERROR_FILE_NOT_FOUND;
            result.hr = HRESULT_FROM_WIN32(result.error);
            result.failed_path = join_path(result.install_dir, required_file);
            result.detail = L"The extracted package is missing ";
            result.detail += required_file;
            return result;
        }
    }

    if (!running_aurora_processes(result.install_dir).empty()) {
        delete_tree(staging, nullptr);
        result.error = ERROR_SHARING_VIOLATION;
        result.hr = HRESULT_FROM_WIN32(result.error);
        result.detail = L"Aurora is still running from the install folder.";
        return result;
    }

    std::wstring backup;
    if (directory_exists(result.install_dir)) {
        backup = result.install_dir + L".old-" + current_timestamp_for_dir();
        if (!MoveFileExW(long_path(result.install_dir).c_str(),
                         long_path(backup).c_str(), MOVEFILE_WRITE_THROUGH)) {
            result.error = GetLastError();
            result.hr = HRESULT_FROM_WIN32(result.error);
            result.detail = L"Replacing the previous install folder failed.";
            result.failed_path = result.install_dir;
            delete_tree(staging, nullptr);
            return result;
        }
        SetFileAttributesW(long_path(backup).c_str(), FILE_ATTRIBUTE_HIDDEN);
    }

    if (!MoveFileExW(long_path(staging).c_str(),
                     long_path(result.install_dir).c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        if (!backup.empty()) {
            MoveFileExW(long_path(backup).c_str(),
                        long_path(result.install_dir).c_str(),
                        MOVEFILE_WRITE_THROUGH);
        }
        delete_tree(staging, nullptr);
        result.error = move_error;
        result.hr = HRESULT_FROM_WIN32(move_error);
        result.detail = L"Moving the new files into the install folder failed.";
        result.failed_path = result.install_dir;
        return result;
    }

    if (!backup.empty()) {
        std::wstring cleanup_error;
        if (!delete_tree(backup, &cleanup_error)) {
            log_line(L"Old install left behind: " + cleanup_error);
        }
    }

    write_registry(options);
    if (!options.update_mode) write_language_marker(options.language);
    result.ok = true;
    result.error = ERROR_SUCCESS;
    result.hr = S_OK;
    result.detail = L"Installed " + result.install_dir;
    return result;
}

} // namespace aurora::setup
