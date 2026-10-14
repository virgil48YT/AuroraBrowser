// Aurora — logging.
//
// A minimal, dependency-free, thread-safe logger. Levels are configurable at
// runtime and via the AURORA_LOG_LEVEL environment variable. Sensitive data
// (URLs, cookies, form input) is never logged at Info or below; Debug-level
// logging of navigation URLs is opt-in and off by default in release builds.
#pragma once

#include <chrono>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace aurora::util {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

class Logger {
public:
    static Logger& instance();

    // Configure from environment (AURORA_LOG_LEVEL). Called once at startup.
    void init_from_environment();

    void set_level(LogLevel level);
    LogLevel level() const;

    // Where log lines are written. Defaults to std::clog. Not owned.
    void set_stream(std::ostream* stream);

    void log(LogLevel level, std::string_view tag, std::string_view message);

    bool enabled(LogLevel level) const { return level >= level_; }

private:
    Logger();
    mutable std::mutex mutex_;
    std::ostream* stream_;
    LogLevel level_ = LogLevel::Info;
};

// RAII helper that builds a log line and flushes it on destruction.
class LogLine {
public:
    LogLine(LogLevel level, std::string_view tag);
    ~LogLine();
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <typename T>
    LogLine& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

private:
    LogLevel level_;
    std::string tag_;
    std::ostringstream buffer_;
};

// Convenience macros so call sites stay terse. AURORA_LOG is checked against
// the global level before formatting the message.
#define AURORA_LOG(level, tag)                              \
    if (!::aurora::util::Logger::instance().enabled(level)) \
        ;                                                    \
    else                                                     \
        ::aurora::util::LogLine(level, tag)

} // namespace aurora::util
