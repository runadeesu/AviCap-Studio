#pragma once
// Structured logging.
//
// Every record carries: UTC timestamp, severity, thread id, category and message.
// Records go to a rotating log file (%LOCALAPPDATA%\AviCapStudio\Logs on
// Windows), optionally stderr, and an in-memory ring buffer shown by the
// Diagnostics panel. Logging is thread-safe and never throws.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace avc {

enum class LogLevel : int { Trace = 0, Debug, Info, Warning, Error, Fatal };

const char* logLevelName(LogLevel l) noexcept;

struct LogRecord {
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::Info;
    uint64_t threadId = 0;
    std::string category;
    std::string message;
};

struct LogConfig {
    std::filesystem::path directory;  // empty: no file output
    LogLevel minLevel = LogLevel::Info;
    bool toStderr = false;
    size_t ringCapacity = 2000;
    int keepFiles = 20;  // older log files are removed at startup
};

namespace log {

void init(const LogConfig& cfg);
void shutdown();
void setMinLevel(LogLevel l);
LogLevel minLevel();
bool enabled(LogLevel l);
void write(LogLevel l, std::string_view category, std::string message);
void flush();
std::vector<LogRecord> recent(size_t maxCount = 500);
std::filesystem::path currentFile();
// Listener invoked for each record (e.g. to surface errors in the UI). Returns id.
int addListener(std::function<void(const LogRecord&)> fn);
void removeListener(int id);
std::string formatRecord(const LogRecord& r);

}  // namespace log

template <typename... Args>
void logf(LogLevel l, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    if (!log::enabled(l)) return;
    try {
        log::write(l, category, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {
        log::write(l, category, "<log format error>");
    }
}

}  // namespace avc

#define AVC_LOG(level, cat, ...) ::avc::logf(::avc::LogLevel::level, cat, __VA_ARGS__)
#define AVC_TRACE(cat, ...) AVC_LOG(Trace, cat, __VA_ARGS__)
#define AVC_DEBUG(cat, ...) AVC_LOG(Debug, cat, __VA_ARGS__)
#define AVC_INFO(cat, ...) AVC_LOG(Info, cat, __VA_ARGS__)
#define AVC_WARN(cat, ...) AVC_LOG(Warning, cat, __VA_ARGS__)
#define AVC_ERROR(cat, ...) AVC_LOG(Error, cat, __VA_ARGS__)
#define AVC_FATAL(cat, ...) AVC_LOG(Fatal, cat, __VA_ARGS__)
