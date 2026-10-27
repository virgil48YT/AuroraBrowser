// Aurora Browser Setup — entry point.
//
// Normal launch shows the wizard. Two headless modes exist for packaging and
// diagnostics:
//   --verify-payload   decompress every entry, verify sizes/CRCs, exit 0/1
//   --update --dir X   jump straight to the progress page (used by Aurora's
//                      own updater; --relaunch launches the browser afterwards)
#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <utility>
#include <vector>

#include "install.hpp"
#include "payload.hpp"
#include "strings.hpp"
#include "ui.hpp"
#include "win_util.hpp"

namespace {

using aurora::setup::Language;
using aurora::setup::PayloadArchive;
using aurora::setup::SetupArgs;

std::vector<std::wstring> arguments() {
    std::vector<std::wstring> result;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return result;
    for (int i = 1; i < count; ++i) result.emplace_back(argv[i]);
    LocalFree(argv);
    return result;
}

bool take_value(const std::vector<std::wstring>& args, std::size_t& index,
                const std::wstring& flag, std::wstring& value) {
    const std::wstring& argument = args[index];
    const std::wstring lower = aurora::setup::lower_ascii(argument);
    if (lower == flag) {
        if (index + 1 >= args.size()) return false;
        value = args[++index];
        return true;
    }
    const std::wstring prefix = flag + L"=";
    if (lower.rfind(prefix, 0) == 0) {
        value = argument.substr(prefix.size());
        return true;
    }
    return false;
}

void parse_components(const std::wstring& value, SetupArgs& args) {
    args.has_components = true;
    args.desktop_shortcut = false;
    args.start_menu_shortcut = false;
    std::wstring lower = aurora::setup::lower_ascii(value);
    std::size_t start = 0;
    while (start <= lower.size()) {
        const std::size_t comma = lower.find(L',', start);
        const std::wstring part =
            lower.substr(start, comma == std::wstring::npos
                                    ? std::wstring::npos
                                    : comma - start);
        if (part == L"desktop" || part == L"desktop_shortcut") {
            args.desktop_shortcut = true;
        } else if (part == L"startmenu" || part == L"start_menu" ||
                   part == L"start") {
            args.start_menu_shortcut = true;
        }
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    if (lower == L"none" || lower.empty()) {
        args.desktop_shortcut = false;
        args.start_menu_shortcut = false;
    }
}

SetupArgs parse_arguments() {
    SetupArgs args;
    const std::vector<std::wstring> list = arguments();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::wstring lower = aurora::setup::lower_ascii(list[i]);
        if (lower == L"--update") {
            args.update_mode = true;
            continue;
        }
        if (lower == L"--relaunch") {
            args.relaunch = true;
            continue;
        }
        if (lower == L"--elevated") {
            args.elevated = true;
            continue;
        }
        if (lower == L"--verify-payload") {
            args.verify_payload = true;
            continue;
        }
        std::wstring value;
        if (take_value(list, i, L"--dir", value) ||
            take_value(list, i, L"--install-dir", value)) {
            args.install_dir = value;
            continue;
        }
        if (take_value(list, i, L"--lang", value) ||
            take_value(list, i, L"--language", value)) {
            args.language = aurora::setup::language_from_tag(value);
            args.has_language = true;
            continue;
        }
        if (take_value(list, i, L"--components", value)) {
            parse_components(value, args);
            continue;
        }
        if (take_value(list, i, L"--log", value)) {
            args.log_path = value;
            continue;
        }
    }
    return args;
}

int verify_payload() {
    PayloadArchive archive;
    std::wstring detail;
    DWORD error = ERROR_SUCCESS;
    bool ok = archive.open(aurora::setup::module_path(), detail, error);
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    if (ok) ok = archive.verify(detail, error, files, bytes);

    std::wstring line;
    if (ok) {
        line = L"OK: payload verified, " +
               std::to_wstring(archive.entries().size()) + L" ZIP entries, " +
               std::to_wstring(files) + L" files, " +
               std::to_wstring(bytes) + L" bytes, all CRCs match.";
    } else {
        if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
        line = L"FAIL: payload verification failed: " +
               aurora::setup::format_hresult(HRESULT_FROM_WIN32(error)) +
               L" " + aurora::setup::format_win32_message(error) + L" " +
               detail;
    }
    aurora::setup::log_line(line);

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output && output != INVALID_HANDLE_VALUE) {
            const std::wstring text = line + L"\r\n";
            DWORD written = 0;
            WriteConsoleW(output, text.c_str(),
                          static_cast<DWORD>(text.size()), &written, nullptr);
        }
        FreeConsole();
    }
    return ok ? 0 : 1;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    // The custom manifest requests PerMonitorV2; this fallback keeps the
    // wizard sharp if the toolchain did not embed it. Resolve the entry point
    // dynamically so the executable still loads on older Windows 10 builds.
    using SetDpiContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_dpi_context = reinterpret_cast<SetDpiContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),
                       "SetProcessDpiAwarenessContext"));
    if (!set_dpi_context ||
        !set_dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    SetupArgs args = parse_arguments();
    aurora::setup::set_log_path(args.log_path);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_initialized = SUCCEEDED(com);
    if (!com_initialized && com != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"COM could not be initialized.",
                    L"Aurora Browser Setup", MB_OK | MB_ICONERROR);
        return 1;
    }

    int result = 1;
    if (args.verify_payload) {
        result = verify_payload();
    } else {
        aurora::setup::SetupWindow window(std::move(args));
        if (window.create(instance)) {
            result = window.run();
        } else {
            MessageBoxW(nullptr, L"The installer window could not be created.",
                        L"Aurora Browser Setup", MB_OK | MB_ICONERROR);
        }
    }

    if (com_initialized) CoUninitialize();
    return result;
}
