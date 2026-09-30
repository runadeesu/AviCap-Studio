#pragma once
// RAM frame cache (LRU with a byte budget). Never holds whole videos: only
// recently decoded / prefetched frames. Lookup is by media time: the cached
// frame whose [pts, pts+duration) covers t.

#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

#include "decode/video_frame.h"

namespace avc {

struct FrameCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    size_t bytes = 0;
    size_t frames = 0;
    size_t budget = 0;
};

class FrameCache {
public:
    explicit FrameCache(size_t budgetBytes = size_t(1) << 30);

    VideoFramePtr find(uint64_t source, Time t);
    void insert(uint64_t source, const VideoFramePtr& frame);
    void eraseSource(uint64_t source);
    void clear();
    void setBudget(size_t bytes);
    // Frees memory down to `targetBytes` (memory-pressure handling).
    void trimTo(size_t targetBytes);
    [[nodiscard]] FrameCacheStats stats() const;

    // Source key for a media file + stream + variant (original, proxy, scaled).
    static uint64_t sourceKey(const std::string& path, int stream, int variant);

private:
    struct Entry {
        VideoFramePtr frame;
        std::list<std::pair<uint64_t, int64_t>>::iterator lru;
    };
    void evictLocked(size_t target);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::map<int64_t, Entry>> bySource_;
    std::list<std::pair<uint64_t, int64_t>> lru_;  // front = most recent
    size_t bytes_ = 0;
    size_t budget_;
    uint64_t hits_ = 0, misses_ = 0;
};

}  // namespace avc
