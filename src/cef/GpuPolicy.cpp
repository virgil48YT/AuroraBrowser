#include "cef/GpuPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace aurora::cef {

namespace {

std::string lowered(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

GpuMode parse_gpu_mode(std::string_view value) {
    const std::string v = lowered(value);
    if (v == "software" || v == "swiftshader" || v == "sw") {
        return GpuMode::Software;
    }
    if (v == "off" || v == "none" || v == "disabled") {
        return GpuMode::Disabled;
    }
    return GpuMode::Hardware;
}

GpuMode gpu_mode_from_environment() {
    if (const char* env = std::getenv("AURORA_GPU")) {
        return parse_gpu_mode(env);
    }
    return GpuMode::Hardware;
}

std::vector<std::string> gpu_command_line_switches(GpuMode mode) {
    std::vector<std::string> out;
    switch (mode) {
        case GpuMode::Hardware:
            // Hardware mode means "let Chromium use hardware acceleration when
            // it considers the driver safe" — NOT "force every risky
            // acceleration path on." Forcing the Vulkan ANGLE backend,
            // zero-copy, gpu-rasterization, WebGL2-compute-context, and
            // ignore-gpu-blocklist onto an AMD driver that Chromium has
            // blocklisted is what crashed the browser within ~1s of showing
            // the window (clean shutdown, no ERROR line — a forced hardware
            // context failing to initialise tears the browser down). So the
            // default keeps only the WebGL intent and a SwiftShader fallback,
            // and lets Chromium pick the stable D3D11 path on Windows or the
            // GL path elsewhere. Users who want the aggressive path can still
            // pass the switches via the command line.
            out.emplace_back("enable-webgl");
            // SwiftShader remains available so that if the GPU process cannot
            // create a hardware context, WebGL degrades to software instead
            // of failing outright.
            out.emplace_back("enable-unsafe-swiftshader");
            // Opt out of Windows DirectComposition. On a number of AMD drivers
            // Chromium's DirectComposition video-processor path fails with
            //   ui/gl/direct_composition_support.cc:616
            //   AMD VideoProcessorGetOutputExtension failed: 0x80070057
            // which corrupts the GPU process (frozen/tearing window, and on
            // some setups a hard GPU-process crash). Disabling DirectComposition
            // keeps hardware GL/WebGL working — it only forgoes the Windows
            // compositor/overlay optimisations — so it is a far better default
            // for a small browser than falling back to `disable-gpu`. The
            // switch is a no-op on platforms where DirectComposition does not
            // exist (Linux/macOS), so it is safe to ship unconditionally.
            out.emplace_back("disable-direct-composition");
            break;
        case GpuMode::Software:
            // Force every GL context onto SwiftShader. `use-gl=angle` with
            // `use-angle=swiftshader` is the supported combination in current
            // Chromium; `enable-unsafe-swiftshader` opts in to WebGL on top of
            // it, and disabling the GPU sandbox is required for the software
            // rasterizer inside minimal containers.
            out.emplace_back("use-gl=angle");
            out.emplace_back("use-angle=swiftshader");
            out.emplace_back("enable-unsafe-swiftshader");
            out.emplace_back("disable-gpu-sandbox");
            out.emplace_back("enable-webgl");
            // Software mode is the safety fallback, so it must not hit the same
            // AMD DirectComposition video-processor failure that hardware mode
            // guards against — see the Hardware case for the full rationale.
            out.emplace_back("disable-direct-composition");
            break;
        case GpuMode::Disabled:
            out.emplace_back("disable-gpu");
            out.emplace_back("disable-gpu-compositing");
            out.emplace_back("disable-software-rasterizer");
            break;
    }
    return out;
}

bool webgl_available(GpuMode mode) { return mode != GpuMode::Disabled; }

std::string_view gpu_mode_name(GpuMode mode) {
    switch (mode) {
        case GpuMode::Hardware: return "hardware (SwiftShader fallback)";
        case GpuMode::Software: return "software (SwiftShader)";
        case GpuMode::Disabled: return "disabled";
    }
    return "hardware";
}

} // namespace aurora::cef
