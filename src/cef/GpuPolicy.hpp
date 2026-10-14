// Aurora — GPU / WebGL policy.
//
// Decides which Chromium command-line switches Aurora passes to CEF so that
// WebGL and GPU compositing work on the widest range of machines:
//
//   * Hardware mode (default): keep GPU rasterization and hardware GL, but
//     ignore Chromium's GPU blocklist (many Linux/VM drivers are blocklisted
//     even though they render fine) and allow SwiftShader as a *fallback* if
//     the sandboxed GPU process cannot get a hardware context.
//   * Software mode: force SwiftShader (Chromium's software GL implementation)
//     for every context. WebGL still works — it is simply rasterized on the
//     CPU. This is what we use on headless machines, inside VMs without a GL
//     driver, and when the user opts in via AURORA_GPU=software.
//   * Disabled mode: no GPU process at all (AURORA_GPU=off). WebGL is then
//     unavailable; kept as an escape hatch for debugging driver crashes.
//
// The policy is deliberately a pure function of (mode) living in the core
// library — no CEF headers — so the exact switch list is unit-tested in CI
// where CEF cannot be downloaded or run. CefLifecycle applies the result in
// OnBeforeCommandLineProcessing.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aurora::cef {

enum class GpuMode {
    Hardware,  // hardware GL with SwiftShader fallback (default)
    Software,  // force SwiftShader software GL
    Disabled,  // no GPU process; WebGL unavailable
};

// Resolve the mode from a value such as the AURORA_GPU environment variable.
// Empty/unknown values resolve to Hardware. Accepted (case-insensitive):
//   "hardware" | "hw" | "gpu"        -> Hardware
//   "software" | "swiftshader" | "sw"-> Software
//   "off" | "none" | "disabled"      -> Disabled
GpuMode parse_gpu_mode(std::string_view value);

// Read AURORA_GPU from the environment and resolve it.
GpuMode gpu_mode_from_environment();

// The Chromium switches for a mode, in the order they are appended. Switches
// are returned as "name" or "name=value" (no leading dashes) which is exactly
// what CefCommandLine::AppendSwitch/AppendSwitchWithValue expect.
std::vector<std::string> gpu_command_line_switches(GpuMode mode);

// True when WebGL is expected to be available in this mode (hardware or
// software rasterization). Used by the settings page and the README table.
bool webgl_available(GpuMode mode);

// Human-readable label for logs and the settings page.
std::string_view gpu_mode_name(GpuMode mode);

} // namespace aurora::cef
