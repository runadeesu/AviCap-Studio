#pragma once
// Copy-on-write editing of the immutable project model.
//
// A ProjectEditor starts from a published snapshot. The first mutable access to
// a node clones it (and its parents); later accesses in the same edit reuse
// the fresh copy. Untouched sequences, tracks and clips remain shared with the
// previous snapshot, so an edit costs O(changed nodes + path to root).

#include <memory>
#include <unordered_set>

#include "timeline/model.h"

namespace avc {

class FreshSet {
public:
    bool contains(const void* p) const { return set_.count(p) != 0; }
    void add(const void* p) { set_.insert(p); }

private:
    std::unordered_set<const void*> set_;
};

template <typename T>
T& makeMutable(std::shared_ptr<const T>& slot, FreshSet& fresh) {
    if (!fresh.contains(slot.get())) {
        auto copy = std::make_shared<T>(*slot);
        fresh.add(copy.get());
        slot = copy;
        return *copy;
    }
    // Objects in the fresh set were created non-const by this edit.
    return const_cast<T&>(*slot);
}

class SequenceEditor {
public:
    SequenceEditor(Sequence& seq, FreshSet& fresh, const Project* project) : seq_(seq), fresh_(fresh), project_(project) {}

    [[nodiscard]] const Sequence& seq() const noexcept { return seq_; }
    Sequence& props() noexcept { return seq_; }
    [[nodiscard]] const Project* project() const noexcept { return project_; }

    [[nodiscard]] int trackCount() const noexcept { return static_cast<int>(seq_.tracks.size()); }
    [[nodiscard]] const Track& trackAt(int index) const { return *seq_.tracks.at(static_cast<size_t>(index)); }
    Track& mutableTrack(int index);
    Track& mutableTrackById(TrackId id);
    Clip& mutableClip(ClipId id);
    Clip& mutableClipAt(int trackIndex, int clipIndex);
    [[nodiscard]] const Clip* clip(ClipId id) const noexcept { return seq_.clip(id); }

    // Inserts keeping the track sorted. The caller guarantees there is no overlap
    // (checked; returns false if it would overlap).
    bool insertClip(int trackIndex, Clip c);
    bool removeClip(ClipId id);
    // Re-sorts the track containing `id` after its start changed.
    void resort(int trackIndex);

    [[nodiscard]] const MediaItem* media(MediaId id) const noexcept {
        return project_ ? project_->findMedia(id) : nullptr;
    }
    // Maximum source time of the clip's source, or nullopt when unlimited
    // (images, titles, solids, adjustment layers).
    [[nodiscard]] std::optional<Time> sourceLimit(const Clip& c) const;

private:
    Sequence& seq_;
    FreshSet& fresh_;
    const Project* project_;
};

class ProjectEditor {
public:
    explicit ProjectEditor(ProjectPtr base);

    [[nodiscard]] const Project& current() const noexcept { return root_ ? *root_ : *base_; }
    [[nodiscard]] const ProjectPtr& base() const noexcept { return base_; }
    Project& root();
    SequenceEditor sequence(SequenceId id);
    MediaItem& media(MediaId id);
    void addMedia(MediaItem item);
    bool removeMedia(MediaId id);
    SequenceId addSequence(Sequence s);
    bool removeSequence(SequenceId id);
    [[nodiscard]] bool changed() const noexcept { return root_ != nullptr; }
    // Finishes the edit. Returns the base snapshot if nothing changed.
    ProjectPtr finish();

private:
    ProjectPtr base_;
    std::shared_ptr<Project> root_;
    FreshSet fresh_;
};

}  // namespace avc
