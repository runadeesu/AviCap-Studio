#include "timeline/editor.h"

#include <algorithm>
#include <stdexcept>

namespace avc {

Track& SequenceEditor::mutableTrack(int index) {
    return makeMutable(seq_.tracks.at(static_cast<size_t>(index)), fresh_);
}

Track& SequenceEditor::mutableTrackById(TrackId id) {
    const int i = seq_.trackIndex(id);
    if (i < 0) throw std::out_of_range("track not found");
    return mutableTrack(i);
}

Clip& SequenceEditor::mutableClip(ClipId id) {
    auto ref = seq_.findClip(id);
    if (!ref) throw std::out_of_range("clip not found");
    return mutableClipAt(ref.track, ref.clip);
}

Clip& SequenceEditor::mutableClipAt(int trackIndex, int clipIndex) {
    Track& t = mutableTrack(trackIndex);
    return makeMutable(t.clips.at(static_cast<size_t>(clipIndex)), fresh_);
}

bool SequenceEditor::insertClip(int trackIndex, Clip c) {
    if (trackIndex < 0 || trackIndex >= trackCount() || c.duration.ticks <= 0) return false;
    const Track& ro = trackAt(trackIndex);
    const size_t pos = ro.lowerBound(c.start);
    // Neighbour checks.
    if (pos < ro.clips.size() && ro.clips[pos]->start < c.end()) return false;
    if (pos > 0 && ro.clips[pos - 1]->end() > c.start) return false;
    Track& t = mutableTrack(trackIndex);
    auto ptr = std::make_shared<Clip>(std::move(c));
    fresh_.add(ptr.get());
    t.clips.insert(t.clips.begin() + static_cast<std::ptrdiff_t>(pos), std::move(ptr));
    return true;
}

bool SequenceEditor::removeClip(ClipId id) {
    auto ref = seq_.findClip(id);
    if (!ref) return false;
    Track& t = mutableTrack(ref.track);
    t.clips.erase(t.clips.begin() + ref.clip);
    return true;
}

void SequenceEditor::resort(int trackIndex) {
    Track& t = mutableTrack(trackIndex);
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
}

std::optional<Time> SequenceEditor::sourceLimit(const Clip& c) const {
    if (c.kind == ClipKind::Media) {
        const MediaItem* m = media(c.media);
        if (!m) return std::nullopt;
        if (m->info.kind == MediaKind::Image) return std::nullopt;
        if (m->info.duration.ticks <= 0) return std::nullopt;
        return m->info.duration;
    }
    if (c.kind == ClipKind::Compound && project_) {
        if (const Sequence* s = project_->findSequence(c.nested)) {
            const Time d = s->duration();
            return d.ticks > 0 ? std::optional<Time>(d) : std::nullopt;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- ProjectEditor

ProjectEditor::ProjectEditor(ProjectPtr base) : base_(std::move(base)) {
    if (!base_) base_ = std::make_shared<Project>();
}

Project& ProjectEditor::root() {
    if (!root_) {
        root_ = std::make_shared<Project>(*base_);
        fresh_.add(root_.get());
    }
    return *root_;
}

SequenceEditor ProjectEditor::sequence(SequenceId id) {
    Project& r = root();
    const int i = r.sequenceIndex(id);
    if (i < 0) throw std::out_of_range("sequence not found");
    Sequence& s = makeMutable(r.sequences[static_cast<size_t>(i)], fresh_);
    return SequenceEditor(s, fresh_, root_.get());
}

MediaItem& ProjectEditor::media(MediaId id) {
    Project& r = root();
    const int i = r.mediaIndex(id);
    if (i < 0) throw std::out_of_range("media not found");
    return makeMutable(r.media[static_cast<size_t>(i)], fresh_);
}

void ProjectEditor::addMedia(MediaItem item) {
    auto p = std::make_shared<MediaItem>(std::move(item));
    fresh_.add(p.get());
    root().media.push_back(std::move(p));
}

bool ProjectEditor::removeMedia(MediaId id) {
    Project& r = root();
    const int i = r.mediaIndex(id);
    if (i < 0) return false;
    r.media.erase(r.media.begin() + i);
    return true;
}

SequenceId ProjectEditor::addSequence(Sequence s) {
    if (s.id == kInvalidId) s.id = newId();
    const SequenceId id = s.id;
    auto p = std::make_shared<Sequence>(std::move(s));
    fresh_.add(p.get());
    Project& r = root();
    r.sequences.push_back(std::move(p));
    if (r.activeSequence == kInvalidId) r.activeSequence = id;
    return id;
}

bool ProjectEditor::removeSequence(SequenceId id) {
    Project& r = root();
    const int i = r.sequenceIndex(id);
    if (i < 0) return false;
    r.sequences.erase(r.sequences.begin() + i);
    if (r.activeSequence == id) r.activeSequence = r.sequences.empty() ? kInvalidId : r.sequences.front()->id;
    return true;
}

ProjectPtr ProjectEditor::finish() {
    if (!root_) return base_;
    ProjectPtr out = root_;
    root_.reset();
    base_ = out;
    fresh_ = FreshSet{};
    return out;
}

}  // namespace avc
