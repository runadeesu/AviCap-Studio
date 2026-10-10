#pragma once
// Thumbnails and waveforms for the UI. Lookups are non-blocking: a miss
// schedules background work (deduplicated, bounded) and returns nothing; the
// wake callback redraws the UI when the result is ready. Thumbnail images are
// uploaded to GPU textures on the UI thread and kept in a small LRU.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "cache/cache.h"
#include "render/gpu/gpu.h"

namespace avc::ui {

struct ThumbnailRef {
    ImTextureID texture = ImTextureID_Invalid;  // invalid when not ready or no GPU view
    int width = 0;
    int height = 0;
    bool ready = false;
    explicit operator bool() const { return ready; }
};

class AssetService {
public:
    AssetService(cache::CacheStore& store, gpu::Device& device, std::function<void()> wake);
    ~AssetService();

    ThumbnailRef thumbnail(const MediaItem& media, Time t, int width = 160);
    std::shared_ptr<const audio::WaveformPeaks> waveform(const MediaItem& media);
    // A texture slot updated in place (scopes, generated images). The upload
    // is skipped when the same image is passed again.
    ImTextureID dynamicTexture(const std::string& slot, const std::shared_ptr<const cache::Image>& image);
    // Preview tile of an effect (its id) or a transition ("transition:<id>").
    // All tiles are rendered once in the background on first use; invalid
    // until ready, or when the effect has no visible default look.
    ImTextureID effectPreview(const std::string& key);
    [[nodiscard]] bool effectPreviewsReady() const;

    void cancelAll();
    void endFrame();  // trims the texture LRU
    [[nodiscard]] size_t pendingJobs() const;
    [[nodiscard]] size_t textureCount() const { return textures_.size(); }
    cache::ThumbnailCache& thumbnails() { return thumbs_; }
    cache::WaveformCache& waveforms() { return waves_; }
    cache::CacheStore& store() { return store_; }

private:
    ImTextureID upload(const cache::ImagePtr& img);

    cache::CacheStore& store_;
    gpu::Device& device_;
    std::function<void()> wake_;
    cache::ThumbnailCache thumbs_;
    cache::WaveformCache waves_;

    mutable std::mutex mutex_;
    std::set<std::string> pending_;          // thumbnail keys in flight
    std::set<MediaId> pendingWaves_;
    std::set<std::string> failed_;           // keys that failed (not retried this session)
    CancelToken group_ = CancelToken::create();
    std::atomic<int> inFlight_{0};

    struct Tex {
        cache::ImagePtr image;  // keeps the address stable while the entry exists
        gpu::TexturePtr tex;
        uint64_t lastUse = 0;
    };
    std::unordered_map<const cache::Image*, Tex> textures_;
    struct Dynamic {
        cache::ImagePtr image;
        gpu::TexturePtr tex;
    };
    std::unordered_map<std::string, Dynamic> dynamic_;
    std::map<std::string, cache::ImagePtr> fxImages_;  // guarded by mutex_
    bool fxRequested_ = false;
    bool fxReady_ = false;                              // guarded by mutex_
    std::map<std::string, gpu::TexturePtr> fxTextures_;  // UI thread
    uint64_t frame_ = 0;
};

}  // namespace avc::ui
