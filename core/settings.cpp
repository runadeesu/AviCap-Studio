#include "core/settings.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "core/file_io.h"
#include "core/log.h"

namespace avc {

using nlohmann::json;

namespace {

template <typename T>
void get(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (...) {
        // Keep default on type mismatch (forward/backward compatibility).
    }
}

}  // namespace

std::string AppSettings::toJson() const {
    json j;
    j["schemaVersion"] = schemaVersion;
    j["general"] = {{"language", general.language},
                    {"autosaveIntervalSec", general.autosaveIntervalSec},
                    {"recentProjects", general.recentProjects},
                    {"defaultProjectDir", general.defaultProjectDir},
                    {"reopenLastProject", general.reopenLastProject}};
    j["interface"] = {{"uiScale", ui.uiScale},
                      {"timecodeStyle", ui.timecodeStyle},
                      {"showTooltips", ui.showTooltips},
                      {"highContrast", ui.highContrast},
                      {"snapping", ui.snapping},
                      {"workspace", ui.workspace},
                      {"multiViewports", ui.multiViewports},
                      {"windowX", ui.windowX},
                      {"windowY", ui.windowY},
                      {"windowW", ui.windowW},
                      {"windowH", ui.windowH},
                      {"windowMaximized", ui.windowMaximized}};
    j["playback"] = {{"previewQuality", playback.previewQuality},
                     {"adaptiveQuality", playback.adaptiveQuality},
                     {"loop", playback.loop},
                     {"audioScrubbing", playback.audioScrubbing},
                     {"shuttleMaxSpeed", playback.shuttleMaxSpeed}};
    j["performance"] = {{"decodeThreads", performance.decodeThreads},
                        {"frameCacheMB", performance.frameCacheMB},
                        {"hardwareDecode", performance.hardwareDecode},
                        {"maxOpenDecoders", performance.maxOpenDecoders}};
    j["gpu"] = {{"api", gpu.api}, {"adapter", gpu.adapter}, {"vramBudgetMB", gpu.vramBudgetMB}};
    j["audio"] = {{"outputDevice", audio.outputDevice}, {"bufferMs", audio.bufferMs}, {"sampleRate", audio.sampleRate}};
    j["proxy"] = {{"useProxies", proxy.useProxies},
                  {"preset", proxy.preset},
                  {"autoCreateAboveHeight", proxy.autoCreateAboveHeight},
                  {"location", proxy.location}};
    j["cache"] = {{"maxGB", cache.maxGB},
                  {"maxAgeDays", cache.maxAgeDays},
                  {"autoCleanup", cache.autoCleanup},
                  {"location", cache.location}};
    j["ai"] = {{"speechProvider", ai.speechProvider},
               {"whisperModelPath", ai.whisperModelPath},
               {"captionLanguage", ai.captionLanguage},
               {"assistantProvider", ai.assistantProvider},
               {"anthropicModel", ai.anthropicModel},
               {"cloudConsent", ai.cloudConsent},
               {"inferenceBackend", ai.inferenceBackend}};
    j["export"] = {{"defaultPreset", exporting.defaultPreset},
                   {"defaultDir", exporting.defaultDir},
                   {"verifyAfterExport", exporting.verifyAfterExport},
                   {"preferHardwareEncoder", exporting.preferHardwareEncoder}};
    j["keyboard"] = {{"preset", keyboard.preset}, {"overrides", keyboard.overrides}};
    j["privacy"] = {{"analyticsOptIn", privacy.analyticsOptIn},
                    {"crashDumps", privacy.crashDumps},
                    {"allowNetwork", privacy.allowNetwork}};
    j["sounds"] = {{"libraryFolders", sounds.libraryFolders},
                   {"favorites", sounds.favorites},
                   {"recent", sounds.recent},
                   {"rootFolder", sounds.rootFolder},
                   {"watchDownloads", sounds.watchDownloads}};
    return j.dump(2);
}

AppSettings AppSettings::fromJson(const std::string& text) {
    AppSettings s;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;
    get(j, "schemaVersion", s.schemaVersion);
    if (auto it = j.find("general"); it != j.end()) {
        get(*it, "language", s.general.language);
        get(*it, "autosaveIntervalSec", s.general.autosaveIntervalSec);
        get(*it, "recentProjects", s.general.recentProjects);
        get(*it, "defaultProjectDir", s.general.defaultProjectDir);
        get(*it, "reopenLastProject", s.general.reopenLastProject);
    }
    if (auto it = j.find("interface"); it != j.end()) {
        get(*it, "uiScale", s.ui.uiScale);
        get(*it, "timecodeStyle", s.ui.timecodeStyle);
        get(*it, "showTooltips", s.ui.showTooltips);
        get(*it, "highContrast", s.ui.highContrast);
        get(*it, "snapping", s.ui.snapping);
        get(*it, "workspace", s.ui.workspace);
        get(*it, "multiViewports", s.ui.multiViewports);
        get(*it, "windowX", s.ui.windowX);
        get(*it, "windowY", s.ui.windowY);
        get(*it, "windowW", s.ui.windowW);
        get(*it, "windowH", s.ui.windowH);
        get(*it, "windowMaximized", s.ui.windowMaximized);
    }
    if (auto it = j.find("playback"); it != j.end()) {
        get(*it, "previewQuality", s.playback.previewQuality);
        get(*it, "adaptiveQuality", s.playback.adaptiveQuality);
        get(*it, "loop", s.playback.loop);
        get(*it, "audioScrubbing", s.playback.audioScrubbing);
        get(*it, "shuttleMaxSpeed", s.playback.shuttleMaxSpeed);
    }
    if (auto it = j.find("performance"); it != j.end()) {
        get(*it, "decodeThreads", s.performance.decodeThreads);
        get(*it, "frameCacheMB", s.performance.frameCacheMB);
        get(*it, "hardwareDecode", s.performance.hardwareDecode);
        get(*it, "maxOpenDecoders", s.performance.maxOpenDecoders);
    }
    if (auto it = j.find("gpu"); it != j.end()) {
        get(*it, "api", s.gpu.api);
        get(*it, "adapter", s.gpu.adapter);
        get(*it, "vramBudgetMB", s.gpu.vramBudgetMB);
    }
    if (auto it = j.find("audio"); it != j.end()) {
        get(*it, "outputDevice", s.audio.outputDevice);
        get(*it, "bufferMs", s.audio.bufferMs);
        get(*it, "sampleRate", s.audio.sampleRate);
    }
    if (auto it = j.find("proxy"); it != j.end()) {
        get(*it, "useProxies", s.proxy.useProxies);
        get(*it, "preset", s.proxy.preset);
        get(*it, "autoCreateAboveHeight", s.proxy.autoCreateAboveHeight);
        get(*it, "location", s.proxy.location);
    }
    if (auto it = j.find("cache"); it != j.end()) {
        get(*it, "maxGB", s.cache.maxGB);
        get(*it, "maxAgeDays", s.cache.maxAgeDays);
        get(*it, "autoCleanup", s.cache.autoCleanup);
        get(*it, "location", s.cache.location);
    }
    if (auto it = j.find("ai"); it != j.end()) {
        get(*it, "speechProvider", s.ai.speechProvider);
        get(*it, "whisperModelPath", s.ai.whisperModelPath);
        get(*it, "captionLanguage", s.ai.captionLanguage);
        get(*it, "assistantProvider", s.ai.assistantProvider);
        get(*it, "anthropicModel", s.ai.anthropicModel);
        get(*it, "cloudConsent", s.ai.cloudConsent);
        get(*it, "inferenceBackend", s.ai.inferenceBackend);
    }
    if (auto it = j.find("export"); it != j.end()) {
        get(*it, "defaultPreset", s.exporting.defaultPreset);
        get(*it, "defaultDir", s.exporting.defaultDir);
        get(*it, "verifyAfterExport", s.exporting.verifyAfterExport);
        get(*it, "preferHardwareEncoder", s.exporting.preferHardwareEncoder);
    }
    if (auto it = j.find("keyboard"); it != j.end()) {
        get(*it, "preset", s.keyboard.preset);
        get(*it, "overrides", s.keyboard.overrides);
    }
    if (auto it = j.find("privacy"); it != j.end()) {
        get(*it, "analyticsOptIn", s.privacy.analyticsOptIn);
        get(*it, "crashDumps", s.privacy.crashDumps);
        get(*it, "allowNetwork", s.privacy.allowNetwork);
    }
    if (auto it = j.find("sounds"); it != j.end()) {
        get(*it, "libraryFolders", s.sounds.libraryFolders);
        get(*it, "favorites", s.sounds.favorites);
        get(*it, "recent", s.sounds.recent);
        get(*it, "rootFolder", s.sounds.rootFolder);
        get(*it, "watchDownloads", s.sounds.watchDownloads);
    }
    // Sanitize ranges.
    s.general.autosaveIntervalSec = std::clamp(s.general.autosaveIntervalSec, 10, 3600);
    s.performance.frameCacheMB = std::clamp(s.performance.frameCacheMB, 128, 65536);
    s.audio.bufferMs = std::clamp(s.audio.bufferMs, 5, 500);
    s.cache.maxGB = std::clamp(s.cache.maxGB, 0.5, 10000.0);
    return s;
}

AppSettings AppSettings::load(const std::filesystem::path& file) {
    auto data = readFileBytes(file);
    if (!data) return AppSettings{};
    return fromJson(*data);
}

Status AppSettings::save(const std::filesystem::path& file) const {
    Status st = writeFileAtomic(file, toJson());
    if (!st) AVC_ERROR("settings", "Failed to save settings: {}", st.message());
    return st;
}

void AppSettings::addRecentProject(const std::string& path, size_t maxCount) {
    auto& v = general.recentProjects;
    v.erase(std::remove(v.begin(), v.end(), path), v.end());
    v.insert(v.begin(), path);
    if (v.size() > maxCount) v.resize(maxCount);
}

}  // namespace avc
