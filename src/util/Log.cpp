#include "util/Log.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

namespace aurora::util {

namespace {
const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?    ";
}

LogLevel parse_level(std::string_view s) {
    if (s == "trace" || s == "TRACE") return LogLevel::Trace;
    if (s == "debug" || s == "DEBUG") return LogLevel::Debug;
    if (s == "warn"  || s == "WARN")  return LogLevel::Warn;
    if (s == "error" || s == "ERROR") return LogLevel::Error;
    return LogLevel::Info;
}
} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() : stream_(&std::clog) {}

void Logger::init_from_environment() {
    if (const char* env = std::getenv("AURORA_LOG_LEVEL")) {
        level_ = parse_level(env);
    }
#ifndef NDEBUG
    level_ = LogLevel::Debug;
#endif
}

void Logger::set_level(LogLevel level) { level_ = level; }
LogLevel Logger::level() const { return level_; }

void Logger::set_stream(std::ostream* stream) { stream_ = stream; }

void Logger::log(LogLevel level, std::string_view tag, std::string_view message) {
    if (level < level_) return;
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    // localtime_r is POSIX; MSVC provides localtime_s with swapped args.
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    // localtime is process-global; use the mutex to stay correct.
    std::lock_guard<std::mutex> lock(mutex_);
    *stream_ << '[';
    // ISO 8601-ish timestamp with seconds precision; microsecond precision
    // would require platform-specific formatting and is not needed for
    // browser diagnostics.
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    *stream_ << buf << "] [" << level_name(level) << "] [" << tag << "] "
             << message << '\n';
    stream_->flush();
}

LogLine::LogLine(LogLevel level, std::string_view tag)
    : level_(level), tag_(tag) {}

LogLine::~LogLine() {
    Logger::instance().log(level_, tag_, buffer_.str());
}

} // namespace aurora::util
