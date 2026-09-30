#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "core/platform.h"
#include "core/strings.h"

namespace avc::log {

namespace {

struct State {
    std::mutex mutex;
    LogConfig config;
    std::FILE* file = nullptr;
    std::filesystem::path filePath;
    std::deque<LogRecord> ring;
    std::map<int, std::function<void(const LogRecord&)>> listeners;
    int nextListener = 1;
};

State& state() {
    static State* s = new State();  // intentionally leaked: usable during static destruction
    return *s;
}

std::atomic<int> g_minLevel{static_cast<int>(LogLevel::Info)};

std::string timestampUtc(std::chrono::system_clock::time_point tp) {
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - secs).count();
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

void pruneOldLogs(const std::filesystem::path& dir, int keep) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> files;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".log") files.push_back(e);
    }
    if (static_cast<int>(files.size()) <= keep) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) < b.last_write_time(e2);
    });
    for (size_t i = 0; i + static_cast<size_t>(keep) < files.size(); ++i) std::filesystem::remove(files[i].path(), ec);
}

}  // namespace

void init(const LogConfig& cfg) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.config = cfg;
    g_minLevel = static_cast<int>(cfg.minLevel);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    if (!cfg.directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.directory, ec);
        pruneOldLogs(cfg.directory, std::max(1, cfg.keepFiles - 1));
        const auto now = std::chrono::system_clock::now();
        std::string stamp = timestampUtc(now);
        for (char& c : stamp)
            if (c == ':' || c == '.') c = '-';
        s.filePath = cfg.directory / pathFromUtf8("avicap-" + stamp + "-" + std::to_string(currentProcessId()) + ".log");
        s.file = openFileUtf8(s.filePath, "ab");
    }
}

void shutdown() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) {
        std::fflush(s.file);
        std::fclose(s.file);
        s.file = nullptr;
    }
}

void setMinLevel(LogLevel l) { g_minLevel = static_cast<int>(l); }
LogLevel minLevel() { return static_cast<LogLevel>(g_minLevel.load()); }
bool enabled(LogLevel l) { return static_cast<int>(l) >= g_minLevel.load(std::memory_order_relaxed); }

std::string formatRecord(const LogRecord& r) {
    std::string line;
    line.reserve(64 + r.category.size() + r.message.size());
    line += timestampUtc(r.time);
    line += " [";
    line += logLevelName(r.level);
    line += "] [t";
    line += std::to_string(r.threadId);
    line += "] [";
    line += r.category;
    line += "] ";
    line += r.message;
    return line;
}

void write(LogLevel l, std::string_view category, std::string message) {
    if (!enabled(l)) return;
    LogRecord rec;
    rec.time = std::chrono::system_clock::now();
    rec.level = l;
    rec.threadId = currentThreadId();
    rec.category = std::string(category);
    rec.message = std::move(message);

    std::vector<std::function<void(const LogRecord&)>> listeners;
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        const std::string line = formatRecord(rec);
        if (s.file) {
            std::fwrite(line.data(), 1, line.size(), s.file);
            std::fputc('\n', s.file);
            if (l >= LogLevel::Warning) std::fflush(s.file);
        }
        if (s.config.toStderr) {
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fputc('\n', stderr);
        }
        s.ring.push_back(rec);
        while (s.ring.size() > std::max<size_t>(16, s.config.ringCapacity)) s.ring.pop_front();
        for (auto& [id, fn] : s.listeners) listeners.push_back(fn);
    }
    for (auto& fn : listeners) {
        try {
            fn(rec);
        } catch (...) {
        }
    }
}

void flush() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) std::fflush(s.file);
}

std::vector<LogRecord> recent(size_t maxCount) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    const size_t n = std::min(maxCount, s.ring.size());
    return std::vector<LogRecord>(s.ring.end() - static_cast<std::ptrdiff_t>(n), s.ring.end());
}

std::filesystem::path currentFile() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.filePath;
}

int addListener(std::function<void(const LogRecord&)> fn) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    const int id = s.nextListener++;
    s.listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.listeners.erase(id);
}

}  // namespace avc::log

namespace avc {

const char* logLevelName(LogLevel l) noexcept {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warning: return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

}  // namespace avc
