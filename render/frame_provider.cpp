#include "render/frame_provider.h"

#include <algorithm>
#include <vector>

#include "core/log.h"

namespace avc {

MediaFrameProvider::MediaFrameProvider(FrameProviderOptions opt) : opt_(opt), cache_(opt.cacheBytes) {}

MediaFrameProvider::~MediaFrameProvider() { closeAllDecoders(); }

void MediaFrameProvider::setProxyResolver(std::function<std::string(const MediaItem&)> resolver) {
    std::lock_guard lock(mutex_);
    proxyResolver_ = std::move(resolver);
}

void MediaFrameProvider::setUseProxies(bool on) {
    std::lock_guard lock(mutex_);
    useProxies_ = on;
}

bool MediaFrameProvider::useProxies() const {
    std::lock_guard lock(mutex_);
    return useProxies_;
}

std::shared_ptr<MediaFrameProvider::Entry> MediaFrameProvider::acquireEntry(const std::string& path, int stream, uint64_t slot) {
    const std::string key = path + "|" + std::to_string(stream) + "|" + std::to_string(slot);
    std::lock_guard lock(mutex_);
    auto it = decoders_.find(key);
    if (it != decoders_.end()) {
        it->second->lastUse = ++useCounter_;
        return it->second;
    }
    // Evict least recently used idle decoders beyond the budget.
    while (static_cast<int>(decoders_.size()) >= std::max(2, opt_.maxOpenDecoders)) {
        auto victim = decoders_.end();
        for (auto d = decoders_.begin(); d != decoders_.end(); ++d) {
            if (d->second.use_count() > 1) continue;  // in use
            if (victim == decoders_.end() || d->second->lastUse < victim->second->lastUse) victim = d;
        }
        if (victim == decoders_.end()) break;
        decoders_.erase(victim);
    }
    auto e = std::make_shared<Entry>();
    e->path = path;
    e->lastUse = ++useCounter_;
    decoders_[key] = e;
    return e;
}

Result<VideoFramePtr> MediaFrameProvider::videoFrame(const MediaItem& media, int streamIndex, Time t, uint64_t slot,
                                                     const CancelToken& cancel) {
    std::string path = media.path;
    int variant = 0;
    {
        std::lock_guard lock(mutex_);
        if (useProxies_ && proxyResolver_) {
            const std::string proxy = proxyResolver_(media);
            if (!proxy.empty()) {
                path = proxy;
                variant = 1;
                streamIndex = -1;  // proxies have a single video stream
            }
        }
    }
    const uint64_t source = FrameCache::sourceKey(path, streamIndex, variant);
    if (media.info.kind != MediaKind::Image)
        if (VideoFramePtr f = cache_.find(source, t)) return f;

    auto entry = acquireEntry(path, streamIndex, media.info.kind == MediaKind::Image ? 0 : slot);
    std::lock_guard lock(entry->mutex);
    if (entry->failed) return Result<VideoFramePtr>::error(entry->error);
    if (!entry->decoder) {
        VideoDecoderOptions o;
        o.streamIndex = streamIndex;
        o.hardware = opt_.hardware;
        o.d3d11Device = opt_.d3d11Device;
        o.fastDecode = opt_.fastDecode;
        auto dec = openVideoDecoder(path, o);
        if (!dec) {
            entry->failed = true;
            entry->error = dec.errorMessage();
            AVC_WARN("render", "Cannot open decoder for '{}': {}", path, entry->error);
            return Result<VideoFramePtr>::error(entry->error);
        }
        entry->decoder = std::move(*dec);
        std::lock_guard l2(mutex_);
        lastDecoderName_ = entry->decoder->decoderName();
    }
    auto frame = entry->decoder->frameAt(t, cancel);
    if (frame && media.info.kind != MediaKind::Image) cache_.insert(source, *frame);
    return frame;
}

void MediaFrameProvider::closeAllDecoders() {
    std::lock_guard lock(mutex_);
    decoders_.clear();
}

void MediaFrameProvider::closeDecodersFor(const std::string& path) {
    std::lock_guard lock(mutex_);
    for (auto it = decoders_.begin(); it != decoders_.end();) {
        if (it->second->path == path) it = decoders_.erase(it);
        else ++it;
    }
}

size_t MediaFrameProvider::openDecoderCount() const {
    std::lock_guard lock(mutex_);
    return decoders_.size();
}

std::string MediaFrameProvider::lastDecoderName() const {
    std::lock_guard lock(mutex_);
    return lastDecoderName_;
}

}  // namespace avc
