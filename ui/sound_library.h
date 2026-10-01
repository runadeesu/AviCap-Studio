#pragma once
// Sound library for BGM and sound effects: local folders (scanned on a worker
// thread), preview playback, favourites/recent, and a legal workflow for
// sounds from websites such as Myinstants.
//
// Myinstants has no official API, so nothing is fetched from it
// automatically: the user opens the site in their browser, downloads a sound
// with the site's own button, and imports the downloaded file here (a copy is
// made; the downloaded original is never moved or deleted).

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/output.h"
#include "core/jobs.h"
#include "core/result.h"

namespace avc::ui {

class App;

struct SoundItem {
    std::string path;        // absolute, UTF-8
    std::string name;        // file name without extension
    std::string category;    // "bgm" | "sfx"
    double durationSec = -1; // -1 while unknown
    int64_t modifiedNs = 0;
    std::string source;      // e.g. "Myinstants" (empty = local file)
    std::string sourceUrl;   // original page given by the user (credit)
};

// Plays one file for previewing (independent of the timeline playback).
class SoundPreviewer {
public:
    explicit SoundPreviewer(bool headless);
    ~SoundPreviewer();
    SoundPreviewer(const SoundPreviewer&) = delete;
    SoundPreviewer& operator=(const SoundPreviewer&) = delete;

    void play(const std::string& path, double maxSeconds = 45.0);
    void stop();
    void tick();  // UI thread: releases the device when playback ended
    [[nodiscard]] bool playing() const;
    [[nodiscard]] const std::string& current() const { return current_; }
    [[nodiscard]] double positionSec() const;
    [[nodiscard]] std::string lastError() const;
    float volume = 0.8f;

private:
    struct Buffer {
        std::mutex mutex;
        std::vector<float> samples;  // interleaved stereo
        std::atomic<size_t> readFrames{0};
        std::atomic<bool> complete{false};
        std::atomic<bool> cancel{false};
        std::string error;
        int rate = 48000;
    };
    bool headless_;
    std::unique_ptr<audio::IAudioOutput> out_;
    std::shared_ptr<Buffer> buf_;
    mutable std::mutex bufMutex_;
    std::thread decoder_;
    std::string current_;
    std::atomic<float> volumeAtomic_{0.8f};
};

class SoundLibrary {
public:
    explicit SoundLibrary(App& app);
    ~SoundLibrary();

    // Library root (settings, default <Music>/AviCap Sounds) with BGM and SFX
    // subfolders; imports are copied here.
    [[nodiscard]] std::filesystem::path rootFolder() const;
    [[nodiscard]] std::vector<std::string> folders() const;
    void rescan();
    [[nodiscard]] bool scanning() const { return scanning_ > 0; }
    [[nodiscard]] const std::vector<SoundItem>& items() const { return items_; }
    [[nodiscard]] const SoundItem* find(const std::string& path) const;

    bool isFavorite(const std::string& path) const;
    void setFavorite(const std::string& path, bool on);
    void markUsed(const std::string& path);  // "recent" list

    // Audio files downloaded in the last `hours` (browser Downloads folder).
    std::vector<SoundItem> recentDownloads(double hours = 24.0) const;
    // Copies a file into the library (SFX/<source> or BGM/<source>) and records
    // where it came from. The original file is left untouched.
    Result<std::string> importFile(const std::string& path, bool music, const std::string& source, const std::string& sourceUrl);

    // Opens the sound site in the user's browser (search is optional).
    static std::string myinstantsUrl(const std::string& search);

    SoundPreviewer& previewer() { return previewer_; }

private:
    void loadSources();
    void saveSources() const;

    App& app_;
    std::vector<SoundItem> items_;
    std::atomic<int> scanning_{0};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::map<std::string, std::pair<std::string, std::string>> sources_;  // file name -> (source, url)
    SoundPreviewer previewer_;
};

}  // namespace avc::ui
