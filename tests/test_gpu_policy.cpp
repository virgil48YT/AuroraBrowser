// The GPU policy decides the Chromium switches that make WebGL work. CEF
// itself cannot be downloaded or run in CI, so the switch list is asserted
// here instead — this is the contract CefLifecycle applies verbatim.
#include <doctest.h>

#include <algorithm>

#include "cef/GpuPolicy.hpp"

using namespace aurora::cef;

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

TEST_CASE("mode parsing covers the documented spellings") {
    CHECK(parse_gpu_mode("hardware") == GpuMode::Hardware);
    CHECK(parse_gpu_mode("HW") == GpuMode::Hardware);
    CHECK(parse_gpu_mode("gpu") == GpuMode::Hardware);
    CHECK(parse_gpu_mode("software") == GpuMode::Software);
    CHECK(parse_gpu_mode("SwiftShader") == GpuMode::Software);
    CHECK(parse_gpu_mode("sw") == GpuMode::Software);
    CHECK(parse_gpu_mode("off") == GpuMode::Disabled);
    CHECK(parse_gpu_mode("none") == GpuMode::Disabled);
    CHECK(parse_gpu_mode("disabled") == GpuMode::Disabled);
}

TEST_CASE("unknown or empty values default to hardware") {
    CHECK(parse_gpu_mode("") == GpuMode::Hardware);
    CHECK(parse_gpu_mode("banana") == GpuMode::Hardware);
}

TEST_CASE("hardware mode keeps webgl + swiftshader fallback but avoids forced risky GPU paths") {
    auto sw = gpu_command_line_switches(GpuMode::Hardware);
    // The intent of hardware mode: WebGL on, SwiftShader kept as a fallback.
    CHECK(has(sw, "enable-webgl"));
    CHECK(has(sw, "enable-unsafe-swiftshader"));
    // DirectComposition is disabled to avoid the AMD video-processor crash
    // (direct_composition_support.cc:616 VideoProcessorGetOutputExtension
    // 0x80070057) while keeping hardware GL/WebGL on.
    CHECK(has(sw, "disable-direct-composition"));
    // Nothing may disable the GPU in hardware mode.
    CHECK_FALSE(has(sw, "disable-gpu"));
    CHECK_FALSE(has(sw, "disable-gpu-compositing"));
    CHECK(webgl_available(GpuMode::Hardware));
    // The aggressive switches that crashed AMD drivers within ~1s of window
    // show (forced Vulkan ANGLE backend, zero-copy, gpu-rasterization,
    // WebGL2-compute-context, ignore-gpu-blocklist) must NOT be forced by
    // default. Hardware mode lets Chromium choose the stable D3D11/GL path.
    CHECK_FALSE(has(sw, "ignore-gpu-blocklist"));
    CHECK_FALSE(has(sw, "enable-gpu-rasterization"));
    CHECK_FALSE(has(sw, "enable-zero-copy"));
    CHECK_FALSE(has(sw, "enable-features=Vulkan,CanvasOopRasterization"));
    CHECK_FALSE(has(sw, "enable-webgl2-compute-context"));
}

TEST_CASE("software mode forces the swiftshader backend, still with webgl") {
    auto sw = gpu_command_line_switches(GpuMode::Software);
    CHECK(has(sw, "use-gl=angle"));
    CHECK(has(sw, "use-angle=swiftshader"));
    CHECK(has(sw, "enable-unsafe-swiftshader"));
    CHECK(has(sw, "enable-webgl"));
    // The safety fallback must also avoid the AMD DirectComposition crash.
    CHECK(has(sw, "disable-direct-composition"));
    CHECK_FALSE(has(sw, "disable-gpu"));
    CHECK(webgl_available(GpuMode::Software));
}

TEST_CASE("disabled mode turns the gpu off and reports webgl unavailable") {
    auto sw = gpu_command_line_switches(GpuMode::Disabled);
    CHECK(has(sw, "disable-gpu"));
    CHECK(has(sw, "disable-gpu-compositing"));
    CHECK_FALSE(has(sw, "enable-webgl"));
    CHECK_FALSE(webgl_available(GpuMode::Disabled));
}

TEST_CASE("switches carry no leading dashes and are never empty") {
    for (auto mode : {GpuMode::Hardware, GpuMode::Software, GpuMode::Disabled}) {
        auto sw = gpu_command_line_switches(mode);
        CHECK_FALSE(sw.empty());
        for (const auto& s : sw) {
            CHECK_FALSE(s.empty());
            CHECK(s.front() != '-');
        }
        CHECK_FALSE(gpu_mode_name(mode).empty());
    }
}
