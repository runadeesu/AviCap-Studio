#include "cache/cache.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/file_io.h"
#include "core/log.h"
#include "core/strings.h"
#include "decode/video_frame.h"

namespace avc::cache {

namespace fs = std::filesystem;

std::string mediaKey(const MediaItem& media) {
    // Use the live file identity so an edited source invalidates derived data
    // even when the project still carries the import-time identity.
    const FileIdentity id = fileIdentity(pathFromUtf8(media.path));
    const uint64_t size = id.exists ? id.size : media.fileSize;
    const int64_t mtime = id.exists ? id.modifiedNs : media.fileModifiedNs;
    return media.path + "|" + std::to_string(size) + "|" + std::to_string(mtime);
}

// ------------------------------------------------------------------ store

CacheStore::CacheStore(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    fs::create_directories(root_, ec);
}

fs::path CacheStore::pathFor(const std::string& category, const std::string& key, const std::string& ext) const {
    return root_ / category / (hex64(fnv1a64(key)) + "." + ext);
}

std::optional<std::string> CacheStore::read(const std::string& category, const std::string& key, const std::string& ext) {
    const fs::path p = pathFor(category, key, ext);
    auto data = readFileBytes(p);
    if (!data) return std::nullopt;
    // Stored entries carry their full key so hash collisions are detected.
    const size_t nl = data->find('\n');
    if (nl == std::string::npos || data->compare(0, nl, key) != 0) return std::nullopt;
    // Touch for LRU cleanup (best effort).
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
    return data->substr(nl + 1);
}

Status CacheStore::write(const std::string& category, const std::string& key, const std::string& ext, const std::string& data) {
    if (key.find('\n') != std::string::npos) return Status::error("cache key must not contain a newline");
    const fs::path p = pathFor(category, key, ext);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return Status::error("cannot create cache directory: " + ec.message());
    std::string blob;
    blob.reserve(key.size() + 1 + data.size());
    blob.append(key).push_back('\n');
    blob.append(data);
    // Cache files are disposable: skip the fsync, keep the atomic rename.
    return writeFileAtomic(p, blob, AtomicWriteOptions{false, false});
}

std::vector<CategoryUsage> CacheStore::usage() const {
    std::vector<CategoryUsage> out;
    std::error_code ec;
    for (fs::directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        CategoryUsage u;
        u.category = pathToUtf8(it->path().filename());
        std::error_code ec2;
        for (fs::recursive_directory_iterator f(it->path(), ec2), fend; !ec2 && f != fend; f.increment(ec2)) {
            std::error_code ec3;
            if (!f->is_regular_file(ec3)) continue;
            u.bytes += f->file_size(ec3);
            ++u.files;
        }
        out.push_back(std::move(u));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
    return out;
}

uint64_t CacheStore::totalBytes() const {
    uint64_t t = 0;
    for (const auto& u : usage()) t += u.bytes;
    return t;
}

uint64_t CacheStore::cleanup(uint64_t maxBytes, int maxAgeDays) {
    struct Entry {
        fs::path path;
        uint64_t size;
        fs::file_time_type time;
    };
    std::vector<Entry> files;
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        Entry e{it->path(), it->file_size(ec2), it->last_write_time(ec2)};
        if (ec2) continue;
        total += e.size;
        files.push_back(std::move(e));
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    const auto now = fs::file_time_type::clock::now();
    const auto maxAge = std::chrono::hours(24) * std::max(0, maxAgeDays);
    uint64_t freed = 0;
    for (const Entry& e : files) {
        const bool tooOld = maxAgeDays > 0 && now - e.time > maxAge;
        const bool overBudget = total - freed > maxBytes;
        if (!tooOld && !overBudget) continue;
        std::error_code ec2;
        if (fs::remove(e.path, ec2)) freed += e.size;
    }
    if (freed) AVC_INFO("cache", "cleanup freed {} bytes in {}", freed, pathToUtf8(root_));
    return freed;
}

uint64_t CacheStore::clearCategory(const std::string& category) {
    if (category.empty() || category.find_first_of("/\\.") != std::string::npos) return 0;
    const fs::path dir = root_ / category;
    uint64_t freed = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2)) freed += it->file_size(ec2);
    }
    fs::remove_all(dir, ec);
    return freed;
}

// -------------------------------------------------------------- thumbnails

namespace {

constexpr size_t kMaxPooledDecoders = 4;

void appendBytes(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

std::string encodeImage(const Image& img) {
    std::string out;
    bool opaque = true;
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 255) {
            opaque = false;
            break;
        }
    // JPEG is ~10x smaller for video frames; PNG keeps transparency.
    if (opaque)
        stbi_write_jpg_to_func(appendBytes, &out, img.width, img.height, 4, img.rgba.data(), 85);
    else
        stbi_write_png_to_func(appendBytes, &out, img.width, img.height, 4, img.rgba.data(), img.width * 4);
    return out;
}

std::shared_ptr<Image> decodeImage(const std::string& data) {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()), static_cast<int>(data.size()), &w, &h,
                                        &n, 4);
    if (!px) return nullptr;
    auto img = std::make_shared<Image>();
    img->width = w;
    img->height = h;
    img->rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(px);
    return img;
}

// Applies display rotation (clockwise degrees) to an RGBA image.
std::shared_ptr<Image> rotate(std::shared_ptr<Image> src, int degrees) {
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees == 0) return src;
    auto dst = std::make_shared<Image>();
    const int w = src->width, h = src->height;
    const bool swap = degrees == 90 || degrees == 270;
    dst->width = swap ? h : w;
    dst->height = swap ? w : h;
    dst->rgba.resize(src->rgba.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int dx = x, dy = y;
            if (degrees == 90) {
                dx = h - 1 - y;
                dy = x;
            } else if (degrees == 180) {
                dx = w - 1 - x;
                dy = h - 1 - y;
            } else if (degrees == 270) {
                dx = y;
                dy = w - 1 - x;
            }
            std::memcpy(&dst->rgba[(static_cast<size_t>(dy) * static_cast<size_t>(dst->width) + static_cast<size_t>(dx)) * 4],
                        &src->rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4], 4);
        }
    return dst;
}

}  // namespace

ThumbnailCache::ThumbnailCache(CacheStore& store, size_t memoryBytes) : store_(store), budget_(memoryBytes) {}

Time ThumbnailCache::quantize(Time t) {
    // 1/4 s steps: dense enough for filmstrips, coarse enough to share entries.
    const int64_t step = kTicksPerSecond / 4;
    return Time{t.ticks >= 0 ? t.ticks / step * step : 0};
}

std::string ThumbnailCache::keyFor(const MediaItem& m, Time t, int maxWidth) const {
    return mediaKey(m) + "|t=" + std::to_string(quantize(t).ticks) + "|w=" + std::to_string(maxWidth);
}

ImagePtr ThumbnailCache::peek(const MediaItem& media, Time t, int maxWidth) {
    const std::string key = keyFor(media, t, maxWidth);
    std::lock_guard lk(mutex_);
    auto it = mem_.find(key);
    if (it == mem_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.second);
    return it->second.first;
}

void ThumbnailCache::trimMemory(size_t bytes) {
    std::lock_guard lk(mutex_);
    while (bytes_ > bytes && !lru_.empty()) {
        auto it = mem_.find(lru_.back());
        if (it != mem_.end()) {
            bytes_ -= it->second.first->rgba.size();
            mem_.erase(it);
        }
        lru_.pop_back();
    }
}

Result<ImagePtr> ThumbnailCache::get(const MediaItem& media, Time t, int maxWidth, const CancelToken& cancel) {
    if (ImagePtr hit = peek(media, t, maxWidth)) return hit;
    if (!media.info.hasVideo()) return Result<ImagePtr>::error("media has no video");
    const std::string key = keyFor(media, t, maxWidth);

    auto remember = [&](std::shared_ptr<Image> img) -> ImagePtr {
        ImagePtr p = std::move(img);
        std::lock_guard lk(mutex_);
        if (auto it = mem_.find(key); it != mem_.end()) return it->second.first;  // raced with another worker
        lru_.push_front(key);
        mem_.emplace(key, std::make_pair(p, lru_.begin()));
        bytes_ += p->rgba.size();
        while (bytes_ > budget_ && lru_.size() > 1) {
            auto victim = mem_.find(lru_.back());
            if (victim != mem_.end()) {
                bytes_ -= victim->second.first->rgba.size();
                mem_.erase(victim);
            }
            lru_.pop_back();
        }
        return p;
    };

    if (auto data = store_.read("thumbnails", key, "img")) {
        if (auto img = decodeImage(*data)) return remember(std::move(img));
    }
    if (cancel.cancelled()) return Result<ImagePtr>::error("cancelled");

    std::shared_ptr<Image> img;
    {
        std::lock_guard lk(decoderMutex_);
        auto it = std::find_if(decoders_.begin(), decoders_.end(),
                               [&](const PooledDecoder& d) { return d.path == media.path && d.maxWidth == maxWidth; });
        if (it == decoders_.end()) {
            VideoDecoderOptions opt;
            opt.maxWidth = maxWidth;
            opt.maxHeight = maxWidth;  // bounds portrait sources too
            opt.threads = 2;
            opt.fastDecode = true;
            auto dec = openVideoDecoder(media.path, opt);
            if (!dec) return Result<ImagePtr>::error(dec.errorMessage());
            decoders_.push_front(PooledDecoder{media.path, maxWidth, std::move(*dec)});
            while (decoders_.size() > kMaxPooledDecoders) decoders_.pop_back();
            it = decoders_.begin();
        } else {
            decoders_.splice(decoders_.begin(), decoders_, it);
            it = decoders_.begin();
        }
        const Time q = quantize(t);
        const Time dur = it->decoder->duration();
        const Time at = dur.ticks > 0 && q >= dur ? Time{std::max<int64_t>(0, dur.ticks - 1)} : q;
        auto frame = it->decoder->frameAt(at, cancel);
        if (!frame) {
            decoders_.erase(it);  // decoder state is unknown after a failure
            return Result<ImagePtr>::error(frame.errorMessage());
        }
        const VideoFrame& f = **frame;
        img = std::make_shared<Image>();
        img->width = f.width;
        img->height = f.height;
        img->rgba.resize(static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 4);
        if (f.format == PixelFormat::RGBA8) {
            for (int y = 0; y < f.height; ++y)
                std::memcpy(&img->rgba[static_cast<size_t>(y) * static_cast<size_t>(f.width) * 4],
                            f.planes[0] + static_cast<ptrdiff_t>(y) * f.strides[0], static_cast<size_t>(f.width) * 4);
        } else {
            convertToRgba8(f, img->rgba.data(), f.width * 4);
        }
        img = rotate(std::move(img), f.rotation);
    }
    if (auto st = store_.write("thumbnails", key, "img", encodeImage(*img)); !st)
        AVC_DEBUG("cache", "thumbnail write failed: {}", st.message());
    return remember(std::move(img));
}

// --------------------------------------------------------------- waveforms

std::shared_ptr<const audio::WaveformPeaks> WaveformCache::peek(const MediaItem& media) {
    std::lock_guard lk(mutex_);
    auto it = mem_.find(mediaKey(media));
    return it == mem_.end() ? nullptr : it->second;
}

Result<std::shared_ptr<const audio::WaveformPeaks>> WaveformCache::get(const MediaItem& media, const CancelToken& cancel,
                                                                        JobContext* progress) {
    using R = Result<std::shared_ptr<const audio::WaveformPeaks>>;
    const std::string key = mediaKey(media);
    {
        std::lock_guard lk(mutex_);
        if (auto it = mem_.find(key); it != mem_.end()) return it->second;
    }
    if (!media.info.hasAudio()) return R::error("media has no audio");
    auto store = [&](audio::WaveformPeaks&& w) {
        auto p = std::make_shared<const audio::WaveformPeaks>(std::move(w));
        std::lock_guard lk(mutex_);
        mem_[key] = p;
        return p;
    };
    if (auto data = store_.read("waveforms", key, "wav.peaks")) {
        if (auto w = audio::WaveformPeaks::deserialize(*data)) return store(std::move(*w));
    }
    auto w = audio::analyzeWaveform(media.path, -1, cancel, progress);
    if (!w) return R::error(w.errorMessage());
    if (auto st = store_.write("waveforms", key, "wav.peaks", w->serialize()); !st)
        AVC_DEBUG("cache", "waveform write failed: {}", st.message());
    return store(std::move(*w));
}

}  // namespace avc::cache
