#pragma once
// Thin OS abstraction. Windows is the product platform; the POSIX branches
// exist so the engine and its tests also run headless on Linux CI.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace avc {

uint32_t currentProcessId();
uint64_t currentThreadId();
void setCurrentThreadName(const char* name);

// fopen with a UTF-8 aware path (uses _wfopen on Windows).
std::FILE* openFileUtf8(const std::filesystem::path& p, const char* mode);

std::filesystem::path executablePath();
std::filesystem::path executableDir();

// Per-user data root:
//   Windows : %LOCALAPPDATA%\AviCapStudio
//   Portable: <exe dir>\UserData   (when "portable.txt" exists next to the exe)
//   POSIX   : $XDG_DATA_HOME/AviCapStudio or ~/.local/share/AviCapStudio
// The AVICAP_DATA_DIR environment variable overrides all of the above.
std::filesystem::path appDataDir();
bool isPortableMode();
std::filesystem::path logsDir();      // <root>/Logs
std::filesystem::path cacheDir();     // <root>/Cache (never contains original media)
std::filesystem::path autosaveDir();  // <root>/Autosave
std::filesystem::path settingsFile(); // <root>/settings.json
std::filesystem::path userDocumentsDir();
std::filesystem::path userMusicDir();
std::filesystem::path userDownloadsDir();
std::filesystem::path tempDir();

struct MemoryStatus {
    uint64_t totalPhysical = 0;
    uint64_t availablePhysical = 0;
    uint64_t processWorkingSet = 0;
    uint64_t processPrivateBytes = 0;
    uint32_t memoryLoadPercent = 0;
};
MemoryStatus queryMemoryStatus();

struct DiskSpace {
    uint64_t capacity = 0;
    uint64_t available = 0;
};
std::optional<DiskSpace> queryDiskSpace(const std::filesystem::path& anyPathOnVolume);

// Process CPU usage in percent of all cores since the previous call.
double processCpuUsagePercent();
unsigned hardwareThreads();
std::string osDescription();
std::string cpuDescription();

// Opens a URL or file with the shell (default browser / associated app).
bool openWithShell(const std::string& urlOrPathUtf8);
// Shows the file selected in Explorer.
bool revealInFileManager(const std::filesystem::path& p);

std::optional<std::string> getEnv(const char* name);

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ".
std::string utcNowIso8601();

}  // namespace avc
