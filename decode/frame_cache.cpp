#include "decode/frame_cache.h"

#include "core/file_io.h"

namespace avc {

FrameCache::FrameCache(size_t budgetBytes) : budget_(budgetBytes) {}

uint64_t FrameCache::sourceKey(const std::string& path, int stream, int variant) {
    uint64_t h = fnv1a64(path);
    h = fnv1a64(std::string_view(reinterpret_cast<const char*>(&stream), sizeof(stream)), h);
    h = fnv1a64(std::string_view(reinterpret_cast<const char*>(&variant), sizeof(variant)), h);
    return h;
}

VideoFramePtr FrameCache::find(uint64_t source, Time t) {
    std::lock_guard lock(mutex_);
    auto sit = bySource_.find(source);
    if (sit != bySource_.end() && !sit->second.empty()) {
        auto& m = sit->second;
        auto it = m.upper_bound(t.ticks);
        if (it != m.begin()) {
            --it;
            if (it->second.frame->covers(t)) {
                lru_.splice(lru_.begin(), lru_, it->second.lru);
                ++hits_;
                return it->second.frame;
            }
        }
    }
    ++misses_;
    return nullptr;
}

void FrameCache::insert(uint64_t source, const VideoFramePtr& frame) {
    if (!frame || frame->isHardware()) return;  // GPU surfaces are cached by the renderer
    std::lock_guard lock(mutex_);
    auto& m = bySource_[source];
    auto it = m.find(frame->pts.ticks);
    if (it != m.end()) {
        bytes_ -= it->second.frame->byteSize();
        it->second.frame = frame;
        bytes_ += frame->byteSize();
        lru_.splice(lru_.begin(), lru_, it->second.lru);
    } else {
        lru_.emplace_front(source, frame->pts.ticks);
        m.emplace(frame->pts.ticks, Entry{frame, lru_.begin()});
        bytes_ += frame->byteSize();
    }
    if (bytes_ > budget_) evictLocked(budget_ - budget_ / 8);
}

void FrameCache::evictLocked(size_t target) {
    while (bytes_ > target && !lru_.empty()) {
        auto [source, pts] = lru_.back();
        lru_.pop_back();
        auto sit = bySource_.find(source);
        if (sit == bySource_.end()) continue;
        auto it = sit->second.find(pts);
        if (it != sit->second.end()) {
            bytes_ -= it->second.frame->byteSize();
            sit->second.erase(it);
        }
        if (sit->second.empty()) bySource_.erase(sit);
    }
}

void FrameCache::eraseSource(uint64_t source) {
    std::lock_guard lock(mutex_);
    auto sit = bySource_.find(source);
    if (sit == bySource_.end()) return;
    for (auto& [pts, e] : sit->second) {
        bytes_ -= e.frame->byteSize();
        lru_.erase(e.lru);
    }
    bySource_.erase(sit);
}

void FrameCache::clear() {
    std::lock_guard lock(mutex_);
    bySource_.clear();
    lru_.clear();
    bytes_ = 0;
}

void FrameCache::setBudget(size_t bytes) {
    std::lock_guard lock(mutex_);
    budget_ = bytes;
    if (bytes_ > budget_) evictLocked(budget_);
}

void FrameCache::trimTo(size_t targetBytes) {
    std::lock_guard lock(mutex_);
    evictLocked(targetBytes);
}

FrameCacheStats FrameCache::stats() const {
    std::lock_guard lock(mutex_);
    FrameCacheStats s;
    s.hits = hits_;
    s.misses = misses_;
    s.bytes = bytes_;
    s.budget = budget_;
    s.frames = lru_.size();
    return s;
}

}  // namespace avc
