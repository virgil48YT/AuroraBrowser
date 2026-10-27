// Aurora Browser Setup — installation orchestration.
//
// The ZIP extraction itself lives in payload.cpp. This layer owns everything
// around it: validating the target, checking free space, staging the new tree,
// atomically replacing the old one, shortcuts, the small HKCU registration the
// updater uses, and launching the bootstrap when the wizard finishes.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "payload.hpp"
#include "strings.hpp"

namespace aurora::setup {

struct InstallOptions {
    std::wstring install_dir;
    std::wstring version;
    Language language = Language::En;
    bool desktop_shortcut = true;
    bool start_menu_shortcut = true;
    bool update_mode = false;
    bool relaunch = false;
    bool elevated = false;
};

struct InstallResult {
    bool ok = false;
    bool canceled = false;
    DWORD error = ERROR_SUCCESS;
    HRESULT hr = S_OK;
    std::wstring detail;
    std::wstring failed_path;
    std::wstring install_dir;
    bool fresh_install = false;
};

InstallResult install_from_payload(PayloadArchive& archive,
                                   const InstallOptions& options,
                                   InstallProgress& progress);

std::wstring default_install_dir();
bool shortcut_exists_for(bool desktop);

// Enumerates processes whose image lives under `install_dir`. The values are
// display strings ("aurora_browser.exe (PID 1234)") ready for a message box.
std::vector<std::wstring> running_aurora_processes(
    const std::wstring& install_dir);
bool wait_for_aurora_exit(const std::wstring& install_dir, int seconds);

bool create_shortcut(bool desktop, const std::wstring& install_dir,
                     std::wstring& detail, HRESULT& error);
bool launch_aurora(const std::wstring& install_dir,
                   const std::wstring& language_tag, bool pass_language,
                   std::wstring& detail, DWORD& error);
bool relaunch_elevated(const std::vector<std::wstring>& args,
                       std::wstring& detail, DWORD& error);

std::uint64_t free_bytes_for_path(const std::wstring& path);

void write_registry(const InstallOptions& options);
void write_language_marker(Language language);
std::wstring language_tag(Language language);

} // namespace aurora::setup
