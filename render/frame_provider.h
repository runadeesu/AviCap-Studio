#pragma once
// Supplies decoded video frames to the compositor.
//
// MediaFrameProvider owns a pool of decoders (one per consumer "slot", usually
// the clip id, so two clips from one file stay sequential) plus the RAM frame
// cache, and resolves proxies when proxy playback is enabled.

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/jobs.h"
#include "core/result.h"
#include "decode/frame_cache.h"
#include "decode/video_decoder.h"
#include "timeline/model.h"

namespace avc {

class IFrameProvider {
public:
    virtual ~IFrameProvider() = default;
    virtual Result<VideoFramePtr> videoFrame(const MediaItem& media, int streamIndex, Time mediaTime, uint64_t slot,
                                             const CancelToken& cancel = {}) = 0;
};

struct FrameProviderOptions {
    size_t cacheBytes = size_t(1) << 30;
    int maxOpenDecoders = 24;
    HwDecodeMode hardware = HwDecodeMode::Off;
    void* d3d11Device = nullptr;
    void (*d3d11Lock)(void*) = nullptr;
    void (*d3d11Unlock)(void*) = nullptr;
    void* d3d11LockCtx = nullptr;
    bool fastDecode = false;
};

class MediaFrameProvider final : public IFrameProvider {
public:
    explicit MediaFrameProvider(FrameProviderOptions opt = {});
    ~MediaFrameProvider() override;

    Result<VideoFramePtr> videoFrame(const MediaItem& media, int streamIndex, Time mediaTime, uint64_t slot,
                                     const CancelToken& cancel = {}) override;

    // Returns a proxy path for a media item or "" to use the original.
    void setProxyResolver(std::function<std::string(const MediaItem&)> resolver);
    void setUseProxies(bool on);
    [[nodiscard]] bool useProxies() const;
    void closeAllDecoders();
    void closeDecodersFor(const std::string& path);
    FrameCache& cache() { return cache_; }
    [[nodiscard]] size_t openDecoderCount() const;
    [[nodiscard]] std::string lastDecoderName() const;

private:
    struct Entry {
        std::mutex mutex;
        std::unique_ptr<IVideoDecoder> decoder;
        std::string path;
        uint64_t lastUse = 0;
        bool failed = false;
        std::string error;
    };
    std::shared_ptr<Entry> acquireEntry(const std::string& path, int stream, uint64_t slot);

    FrameProviderOptions opt_;
    FrameCache cache_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> decoders_;
    uint64_t useCounter_ = 0;
    std::function<std::string(const MediaItem&)> proxyResolver_;
    bool useProxies_ = false;
    std::string lastDecoderName_;
};

}  // namespace avc
