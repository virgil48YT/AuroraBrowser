// Aurora — performance instrumentation.
//
// A tiny scoped timer and a counter registry. The goal is not to be a
// profiler — Chromium/CEF already have excellent ones — but to give the
// browser process a cheap, always-on way to record startup time, tab-open
// latency, and navigation responsiveness against the performance budgets
// defined in docs/performance-budgets.md.
//
// Records are emitted as structured log lines so they can be grepped or
// forwarded to the platform's tracing infrastructure later.
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace aurora::util {

class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view label);
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    int64_t elapsed_micros() const;

private:
    std::string label_;
    std::chrono::steady_clock::time_point start_;
};

// Record a named timing sample (microseconds). Logged at Info level so it is
// visible without a debug build.
void record_timing(std::string_view label, int64_t micros);

// Record a startup milestone. Milestones are logged in order and the total
// startup duration is derived from the first to the last.
void startup_milestone(std::string_view name);

} // namespace aurora::util
