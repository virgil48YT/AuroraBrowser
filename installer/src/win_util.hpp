// Aurora Browser Setup — small Win32 helpers shared by the installer.
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::setup {

std::wstring module_path();
std::wstring exe_directory();
std::wstring join_path(std::wstring_view base, std::wstring_view leaf);
std::wstring normalize_path(const std::wstring& path);
std::wstring long_path(const std::wstring& path);
std::wstring parent_directory(const std::wstring& path);
std::wstring file_name(const std::wstring& path);
std::wstring trim_copy(const std::wstring& text);
std::wstring lower_ascii(std::wstring text);
bool is_absolute_path(const std::wstring& path);
bool is_safe_install_dir(const std::wstring& path, std::wstring& why);
bool path_is_under(const std::wstring& path, const std::wstring& directory);

bool file_exists(const std::wstring& path);
bool directory_exists(const std::wstring& path);
bool ensure_directory(const std::wstring& path, DWORD& error);
uint64_t file_size_win32(const std::wstring& path, DWORD& error);
uint64_t directory_size(const std::wstring& path);
bool delete_tree(const std::wstring& path, std::wstring* first_error);

std::wstring format_win32_message(DWORD error);
std::wstring format_hresult(HRESULT hr);
std::wstring format_bytes(uint64_t bytes);
std::wstring get_known_folder(REFKNOWNFOLDERID id);
std::wstring roaming_aurora_dir();

std::wstring quote_arg(const std::wstring& arg);
std::wstring command_line_for(const std::wstring& exe,
                              const std::vector<std::wstring>& args);

bool process_is_elevated();

void set_log_path(const std::wstring& path);
const std::wstring& log_path();
void log_line(const std::wstring& line);

} // namespace aurora::setup
