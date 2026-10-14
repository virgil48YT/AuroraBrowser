#include "util/Perf.hpp"

#include "util/Log.hpp"

#include <chrono>

namespace aurora::util {

ScopedTimer::ScopedTimer(std::string_view label)
    : label_(label), start_(std::chrono::steady_clock::now()) {}

ScopedTimer::~ScopedTimer() {
    auto end = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                       end - start_)
                       .count();
    record_timing(label_, micros);
}

int64_t ScopedTimer::elapsed_micros() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now - start_)
        .count();
}

void record_timing(std::string_view label, int64_t micros) {
    // Logged at Debug to avoid spamming release logs; the perf budget
    // harness can bump the level when running benchmarks.
    AURORA_LOG(LogLevel::Debug, "perf") << label << " " << micros << "us";
}

static std::chrono::steady_clock::time_point startup_start() {
    static const auto t = std::chrono::steady_clock::now();
    return t;
}

void startup_milestone(std::string_view name) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - startup_start())
                  .count();
    AURORA_LOG(LogLevel::Info, "startup") << name << " +" << ms << "ms";
}

} // namespace aurora::util
