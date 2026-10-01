#include "ui/assets.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "core/log.h"

namespace avc::ui {

namespace {
constexpr int kMaxInFlight = 48;
constexpr size_t kMaxTextures = 768;

std::string thumbKey(const MediaItem& m, Time t, int width) {
    return std::to_string(m.id) + "|" + m.path + "|" + std::to_string(cache::ThumbnailCache::quantize(t).ticks) + "|" +
           std::to_string(width);
}
}  // namespace

AssetService::AssetService(cache::CacheStore& store, gpu::Device& device, std::function<void()> wake)
    : store_(store), device_(device), wake_(std::move(wake)), thumbs_(store), waves_(store) {}

AssetService::~AssetService() {
    group_.cancel();
    // Jobs reference this object: wait for in-flight work to observe cancellation.
    for (int i = 0; i < 500 && inFlight_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void AssetService::cancelAll() {
    std::lock_guard lk(mutex_);
    group_.cancel();
    group_ = CancelToken::create();
    pending_.clear();
    pendingWaves_.clear();
}

size_t AssetService::pendingJobs() const { return static_cast<size_t>(inFlight_.load()); }

ThumbnailRef AssetService::thumbnail(const MediaItem& media, Time t, int width) {
    ThumbnailRef ref;
    if (!media.info.hasVideo()) return ref;
    if (cache::ImagePtr img = thumbs_.peek(media, t, width)) {
        ref.ready = true;
        ref.width = img->width;
        ref.height = img->height;
        ref.texture = upload(img);
        return ref;
    }
    const std::string key = thumbKey(media, t, width);
    CancelToken token;
    {
        std::lock_guard lk(mutex_);
        if (pending_.count(key) || failed_.count(key)) return ref;
        if (inFlight_.load() >= kMaxInFlight) return ref;  // retried on a later frame while still visible
        pending_.insert(key);
        token = group_.child();
    }
    ++inFlight_;
    MediaItem copy = media;
    Jobs::cache().submit("Thumbnail", JobPriority::High, [this, copy, t, width, key, token](JobContext&) {
        auto r = thumbs_.get(copy, t, width, token);
        {
            std::lock_guard lk(mutex_);
            pending_.erase(key);
            if (!r && !token.cancelled()) {
                failed_.insert(key);
                AVC_DEBUG("ui", "thumbnail failed for {}: {}", copy.name, r.errorMessage());
            }
        }
        --inFlight_;
        if (r && wake_) wake_();
    }, token);
    return ref;
}

std::shared_ptr<const audio::WaveformPeaks> AssetService::waveform(const MediaItem& media) {
    if (!media.info.hasAudio()) return nullptr;
    if (auto w = waves_.peek(media)) return w;
    CancelToken token;
    {
        std::lock_guard lk(mutex_);
        if (pendingWaves_.count(media.id)) return nullptr;
        pendingWaves_.insert(media.id);
        token = group_.child();
    }
    ++inFlight_;
    MediaItem copy = media;
    Jobs::cache().submit("Waveform " + media.name, JobPriority::Medium, [this, copy, token](JobContext& ctx) {
        auto r = waves_.get(copy, token, &ctx);
        if (!r && !token.cancelled()) AVC_WARN("ui", "waveform failed for {}: {}", copy.name, r.errorMessage());
        {
            std::lock_guard lk(mutex_);
            // Keep failed items marked as pending so they are not retried every frame.
            if (r || token.cancelled()) pendingWaves_.erase(copy.id);
        }
        --inFlight_;
        if (r && wake_) wake_();
    }, token);
    return nullptr;
}

ImTextureID AssetService::upload(const cache::ImagePtr& img) {
    auto it = textures_.find(img.get());
    if (it != textures_.end()) {
        it->second.lastUse = frame_;
        void* view = device_.nativeView(*it->second.tex);
        return view ? reinterpret_cast<ImTextureID>(view) : ImTextureID_Invalid;
    }
    gpu::TextureDesc d;
    d.width = img->width;
    d.height = img->height;
    d.format = gpu::TexFormat::RGBA8;
    d.renderTarget = false;
    gpu::TexturePtr tex = device_.createTexture(d);
    if (!tex) return ImTextureID_Invalid;
    device_.upload(*tex, img->rgba.data(), img->width * 4);
    textures_[img.get()] = Tex{img, tex, frame_};
    void* view = device_.nativeView(*tex);
    return view ? reinterpret_cast<ImTextureID>(view) : ImTextureID_Invalid;
}

ImTextureID AssetService::dynamicTexture(const std::string& slot, const std::shared_ptr<const cache::Image>& image) {
    if (!image || image->width <= 0 || image->height <= 0) return ImTextureID_Invalid;
    Dynamic& d = dynamic_[slot];
    if (!d.tex || d.tex->width() != image->width || d.tex->height() != image->height) {
        gpu::TextureDesc desc;
        desc.width = image->width;
        desc.height = image->height;
        desc.format = gpu::TexFormat::RGBA8;
        desc.renderTarget = false;
        d.tex = device_.createTexture(desc);
        d.image.reset();
        if (!d.tex) return ImTextureID_Invalid;
    }
    if (d.image != image) {
        device_.upload(*d.tex, image->rgba.data(), image->width * 4);
        d.image = image;
    }
    void* view = device_.nativeView(*d.tex);
    return view ? reinterpret_cast<ImTextureID>(view) : ImTextureID_Invalid;
}

void AssetService::endFrame() {
    ++frame_;
    if (textures_.size() <= kMaxTextures) return;
    std::vector<std::pair<uint64_t, const cache::Image*>> order;
    order.reserve(textures_.size());
    for (const auto& [k, v] : textures_) order.emplace_back(v.lastUse, k);
    std::sort(order.begin(), order.end());
    // Never drop textures used in the current or previous frame (still referenced by draw data).
    for (const auto& [use, key] : order) {
        if (textures_.size() <= kMaxTextures * 3 / 4 || use + 2 >= frame_) break;
        textures_.erase(key);
    }
}

}  // namespace avc::ui
