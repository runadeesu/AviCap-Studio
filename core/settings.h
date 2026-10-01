#pragma once
// Application settings (persisted as JSON in the per-user data directory).

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace avc {

struct GeneralSettings {
    std::string language = "auto";  // auto | en | ja
    int autosaveIntervalSec = 60;
    std::vector<std::string> recentProjects;
    std::string defaultProjectDir;
    bool reopenLastProject = false;
};

struct InterfaceSettings {
    float uiScale = 0.0f;  // 0 = follow Windows DPI
    std::string timecodeStyle = "timecode";  // timecode | frames | seconds
    bool showTooltips = true;
    bool highContrast = false;  // also enabled automatically when Windows high contrast is on
    bool snapping = true;
    std::string workspace = "Editing";
    bool multiViewports = true;  // panels can be dragged out to other monitors
    // Main window placement (restored at startup).
    int windowX = -1, windowY = -1, windowW = 0, windowH = 0;
    bool windowMaximized = true;
};

struct PlaybackSettings {
    std::string previewQuality = "auto";  // full | half | quarter | eighth | auto
    bool adaptiveQuality = true;
    bool loop = false;
    bool audioScrubbing = true;
    int shuttleMaxSpeed = 8;
};

struct PerformanceSettings {
    int decodeThreads = 0;       // 0 = auto
    int frameCacheMB = 1536;     // RAM budget for decoded frames
    bool hardwareDecode = true;
    int maxOpenDecoders = 24;
};

struct GpuSettings {
    std::string api = "auto";  // auto | d3d11 | d3d12 | warp
    std::string adapter;       // empty = default / highest performance
    int vramBudgetMB = 0;      // 0 = auto (from DXGI budget)
};

struct AudioSettings {
    std::string outputDevice;  // empty = system default
    int bufferMs = 30;
    int sampleRate = 48000;
};

struct ProxySettings {
    bool useProxies = true;
    std::string preset = "540p";  // 360p | 540p | 720p | 1080p
    int autoCreateAboveHeight = 0;  // 0 = never automatically
    std::string location;  // empty = cache dir
};

struct CacheSettings {
    double maxGB = 20.0;
    int maxAgeDays = 30;
    bool autoCleanup = true;
    std::string location;  // empty = default
};

struct AiSettings {
    std::string speechProvider = "whisper-local";
    std::string whisperModelPath;       // ggml model file
    std::string captionLanguage = "auto";
    std::string assistantProvider = "local";  // local | anthropic
    std::string anthropicModel = "claude-opus-5-5";
    bool cloudConsent = false;  // explicit opt-in before any upload
    std::string inferenceBackend = "auto";  // auto | cpu | directml
};

struct ExportSettings {
    std::string defaultPreset = "youtube-1080p";
    std::string defaultDir;
    bool verifyAfterExport = true;
    bool preferHardwareEncoder = true;
};

struct KeyboardSettings {
    std::string preset = "avicap";  // avicap | premiere | resolve
    std::map<std::string, std::string> overrides;  // action id -> shortcut string
};

struct PrivacySettings {
    bool analyticsOptIn = false;  // no analytics are collected unless enabled
    bool crashDumps = true;
    bool allowNetwork = true;  // sound providers / cloud AI
};

struct SoundsSettings {
    std::vector<std::string> libraryFolders;
    std::vector<std::string> favorites;  // sound ids
    std::vector<std::string> recent;
};

struct AppSettings {
    int schemaVersion = 1;
    GeneralSettings general;
    InterfaceSettings ui;
    PlaybackSettings playback;
    PerformanceSettings performance;
    GpuSettings gpu;
    AudioSettings audio;
    ProxySettings proxy;
    CacheSettings cache;
    AiSettings ai;
    ExportSettings exporting;
    KeyboardSettings keyboard;
    PrivacySettings privacy;
    SoundsSettings sounds;

    static AppSettings load(const std::filesystem::path& file);
    Status save(const std::filesystem::path& file) const;
    std::string toJson() const;
    static AppSettings fromJson(const std::string& json);

    void addRecentProject(const std::string& path, size_t maxCount = 12);
};

}  // namespace avc
