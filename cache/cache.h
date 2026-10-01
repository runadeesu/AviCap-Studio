#pragma once
// Disk + memory caches for derived data (thumbnails, waveforms, proxies,
// shaders, analysis). The cache lives in its own directory and never contains
// original media, so deleting it is always safe.
//
// Keys always include the source identity (path + size + modification time),
// so a changed source file transparently invalidates its derived data.

#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/waveform.h"
#include "core/jobs.h"
#include "core/result.h"
#include "decode/video_decoder.h"
#include "timeline/model.h"

namespace avc::cache {

// Stable key for a media item's content (path + size + mtime).
std::string mediaKey(const MediaItem& media);

struct CategoryUsage {
    std::string category;
    uint64_t bytes = 0;
    uint64_t files = 0;
};

class CacheStore {
public:
    explicit CacheStore(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::filesystem::path pathFor(const std::string& category, const std::string& key, const std::string& ext) const;
    std::optional<std::string> read(const std::string& category, const std::string& key, const std::string& ext);
    Status write(const std::string& category, const std::string& key, const std::string& ext, const std::string& data);
    [[nodiscard]] std::vector<CategoryUsage> usage() const;
    [[nodiscard]] uint64_t totalBytes() const;
    // Removes files older than maxAgeDays and then least-recently-used files
    // until the total is below maxBytes. Returns bytes freed.
    uint64_t cleanup(uint64_t maxBytes, int maxAgeDays);
    uint64_t clearCategory(const std::string& category);

private:
    std::filesystem::path root_;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // tightly packed RGBA8
};
using ImagePtr = std::shared_ptr<const Image>;

class ThumbnailCache {
public:
    explicit ThumbnailCache(CacheStore& store, size_t memoryBytes = 64u << 20);
    // Thumbnail of `media` at time `t` (quantized to `step`), fitting `maxWidth`.
    Result<ImagePtr> get(const MediaItem& media, Time t, int maxWidth = 160, const CancelToken& cancel = {});
    // Memory-only lookup (UI thread, never blocks on decoding).
    ImagePtr peek(const MediaItem& media, Time t, int maxWidth = 160);
    static Time quantize(Time t);
    void trimMemory(size_t bytes);

private:
    std::string keyFor(const MediaItem& m, Time t, int maxWidth) const;
    CacheStore& store_;
    size_t budget_;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::pair<ImagePtr, std::list<std::string>::iterator>> mem_;
    size_t bytes_ = 0;
    // Small pool of downscaling decoders (most recently used first) so
    // consecutive thumbnails of the same file do not reopen it.
    std::mutex decoderMutex_;
    struct PooledDecoder {
        std::string path;
        int maxWidth = 0;
        std::unique_ptr<IVideoDecoder> decoder;
    };
    std::list<PooledDecoder> decoders_;
};

class WaveformCache {
public:
    explicit WaveformCache(CacheStore& store) : store_(store) {}
    std::shared_ptr<const audio::WaveformPeaks> peek(const MediaItem& media);
    Result<std::shared_ptr<const audio::WaveformPeaks>> get(const MediaItem& media, const CancelToken& cancel = {},
                                                             JobContext* progress = nullptr);

private:
    CacheStore& store_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const audio::WaveformPeaks>> mem_;
};

}  // namespace avc::cache
