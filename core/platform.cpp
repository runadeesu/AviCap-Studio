#include "core/platform.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <thread>

#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <fstream>
#endif

namespace avc {

uint32_t currentProcessId() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<uint32_t>(getpid());
#endif
}

uint64_t currentThreadId() {
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(syscall(SYS_gettid));
#endif
}

void setCurrentThreadName(const char* name) {
#if defined(_WIN32)
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto fn = reinterpret_cast<SetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (fn) fn(GetCurrentThread(), utf8ToWide(name).c_str());
#else
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
#endif
}

std::FILE* openFileUtf8(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
    std::wstring wmode;
    for (const char* m = mode; *m; ++m) wmode.push_back(static_cast<wchar_t>(*m));
    return _wfopen(p.c_str(), wmode.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

std::filesystem::path executablePath() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return std::filesystem::path(buf);
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
#endif
}

std::filesystem::path executableDir() { return executablePath().parent_path(); }

std::optional<std::string> getEnv(const char* name) {
#if defined(_WIN32)
    const std::wstring wname = utf8ToWide(name);
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring v(n, L'\0');
    const DWORD m = GetEnvironmentVariableW(wname.c_str(), v.data(), n);
    v.resize(m);
    return wideToUtf8(v);
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

bool isPortableMode() {
    static const bool portable = [] {
        std::error_code ec;
        return std::filesystem::exists(executableDir() / "portable.txt", ec);
    }();
    return portable;
}

std::filesystem::path appDataDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path root;
        if (auto env = getEnv("AVICAP_DATA_DIR"); env && !env->empty()) {
            root = pathFromUtf8(*env);
        } else if (isPortableMode()) {
            root = executableDir() / "UserData";
        } else {
#if defined(_WIN32)
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
                root = std::filesystem::path(path) / L"AviCapStudio";
                CoTaskMemFree(path);
            } else {
                root = std::filesystem::temp_directory_path() / "AviCapStudio";
            }
#else
            if (auto xdg = getEnv("XDG_DATA_HOME"); xdg && !xdg->empty())
                root = pathFromUtf8(*xdg) / "AviCapStudio";
            else if (auto home = getEnv("HOME"))
                root = pathFromUtf8(*home) / ".local" / "share" / "AviCapStudio";
            else
                root = std::filesystem::temp_directory_path() / "AviCapStudio";
#endif
        }
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        return root;
    }();
    return dir;
}

namespace {
std::filesystem::path ensureDir(std::filesystem::path p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}
}  // namespace

std::filesystem::path logsDir() { return ensureDir(appDataDir() / "Logs"); }
std::filesystem::path cacheDir() { return ensureDir(appDataDir() / "Cache"); }
std::filesystem::path autosaveDir() { return ensureDir(appDataDir() / "Autosave"); }
std::filesystem::path settingsFile() { return appDataDir() / "settings.json"; }

namespace {
#if defined(_WIN32)
std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::filesystem::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        r = std::filesystem::path(path);
        CoTaskMemFree(path);
    }
    return r;
}
#else
std::filesystem::path homeSubdir(const char* name) {
    if (auto home = getEnv("HOME")) return pathFromUtf8(*home) / name;
    return std::filesystem::temp_directory_path();
}
#endif
}  // namespace

std::filesystem::path userDocumentsDir() {
#if defined(_WIN32)
    return knownFolder(FOLDERID_Documents);
#else
    if (auto home = getEnv("HOME")) return pathFromUtf8(*home);
    return std::filesystem::temp_directory_path();
#endif
}

std::filesystem::path userMusicDir() {
#if defined(_WIN32)
    return knownFolder(FOLDERID_Music);
#else
    return homeSubdir("Music");
#endif
}

std::filesystem::path userDownloadsDir() {
#if defined(_WIN32)
    return knownFolder(FOLDERID_Downloads);
#else
    return homeSubdir("Downloads");
#endif
}

std::filesystem::path tempDir() { return ensureDir(appDataDir() / "Temp"); }

MemoryStatus queryMemoryStatus() {
    MemoryStatus s;
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.totalPhysical = ms.ullTotalPhys;
        s.availablePhysical = ms.ullAvailPhys;
        s.memoryLoadPercent = ms.dwMemoryLoad;
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.processWorkingSet = pmc.WorkingSetSize;
        s.processPrivateBytes = pmc.PrivateUsage;
    }
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemTotal:") s.totalPhysical = value * 1024;
        else if (key == "MemAvailable:") s.availablePhysical = value * 1024;
    }
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (startsWith(line, "VmRSS:")) s.processWorkingSet = std::strtoull(line.c_str() + 6, nullptr, 10) * 1024;
        if (startsWith(line, "VmData:")) s.processPrivateBytes = std::strtoull(line.c_str() + 7, nullptr, 10) * 1024;
    }
    if (s.totalPhysical)
        s.memoryLoadPercent =
            static_cast<uint32_t>(100 - (s.availablePhysical * 100 / std::max<uint64_t>(1, s.totalPhysical)));
#endif
    return s;
}

std::optional<DiskSpace> queryDiskSpace(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path probe = p;
    while (!probe.empty() && !std::filesystem::exists(probe, ec)) {
        auto parent = probe.parent_path();
        if (parent == probe) break;
        probe = parent;
    }
    auto info = std::filesystem::space(probe.empty() ? std::filesystem::current_path(ec) : probe, ec);
    if (ec) return std::nullopt;
    return DiskSpace{info.capacity, info.available};
}

double processCpuUsagePercent() {
    static uint64_t lastProc = 0, lastWall = 0;
#if defined(_WIN32)
    FILETIME create, exit, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    auto toU64 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const uint64_t proc = toU64(kernel) + toU64(user);
    const uint64_t wall = toU64(now);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const uint64_t proc = (uint64_t(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
                           uint64_t(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec)) * 10;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t wall = (uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec)) / 100;
#endif
    double pct = 0;
    if (lastWall && wall > lastWall) {
        pct = 100.0 * double(proc - lastProc) / double(wall - lastWall) / double(hardwareThreads());
    }
    lastProc = proc;
    lastWall = wall;
    return pct;
}

unsigned hardwareThreads() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

std::string osDescription() {
#if defined(_WIN32)
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn && fn(&vi) == 0) {
        const char* name = vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
        return std::string(name) + " (build " + std::to_string(vi.dwBuildNumber) + ")";
    }
    return "Windows";
#else
    utsname u{};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "POSIX";
#endif
}

std::string cpuDescription() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        for (int i = 0; i < 3; ++i) {
            __cpuid(regs, 0x80000002 + i);
            std::memcpy(brand + i * 16, regs, 16);
        }
        return trim(brand) + " (" + std::to_string(hardwareThreads()) + " threads)";
    }
    return std::to_string(hardwareThreads()) + " threads";
#else
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (startsWith(line, "model name")) {
            auto pos = line.find(':');
            if (pos != std::string::npos)
                return trim(line.substr(pos + 1)) + " (" + std::to_string(hardwareThreads()) + " threads)";
        }
    }
    return std::to_string(hardwareThreads()) + " threads";
#endif
}

bool openWithShell(const std::string& target) {
#if defined(_WIN32)
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", utf8ToWide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
#else
    const std::string cmd = "xdg-open '" + replaceAll(target, "'", "'\\''") + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

bool revealInFileManager(const std::filesystem::path& p) {
#if defined(_WIN32)
    const std::wstring args = L"/select,\"" + p.wstring() + L"\"";
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    return r > 32;
#else
    return openWithShell(pathToUtf8(p.parent_path()));
#endif
}

std::string utcNowIso8601() {
    const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace avc
