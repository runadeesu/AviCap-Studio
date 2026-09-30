#include "timeline/edit_ops.h"

#include <algorithm>
#include <map>

namespace avc::edit {

namespace {

constexpr Time kInfinite = Time::max();

Id deriveId(Id a, int64_t b) {
    uint64_t z = a ^ (static_cast<uint64_t>(b) * 0x9E3779B97F4A7C15ull) ^ 0xA5A5A5A5DEADBEEFull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z == 0 ? 1 : z;
}

Time inverseScale(Time t, Rational speed) {
    if (speed.num <= 0) return kInfinite;
    return t.scaled(speed.inverse(), Rounding::Down);
}

void shiftClipKeys(Clip& c, Time delta) {
    if (delta.ticks == 0) return;
    c.transform.shiftAllKeys(delta);
    c.audio.shiftAllKeys(delta);
    c.textParams.shiftAllKeys(delta);
    for (auto& fx : c.effects) fx.params.shiftAllKeys(delta);
}

// Maximum distance the clip's head can move left (source-wise).
Time maxHeadExtension(const SequenceEditor& e, const Clip& c) {
    if (c.freezeFrame || !c.hasLimitedSource()) return kInfinite;
    if (!c.reverse) return inverseScale(c.sourceIn, c.speed);
    auto limit = e.sourceLimit(c);
    if (!limit) return kInfinite;
    const Time room = *limit - c.sourceOut();
    return room.ticks > 0 ? inverseScale(room, c.speed) : Time{0};
}

// Maximum distance the clip's tail can move right (source-wise).
Time maxTailExtension(const SequenceEditor& e, const Clip& c) {
    if (c.freezeFrame || !c.hasLimitedSource()) return kInfinite;
    if (c.reverse) return inverseScale(c.sourceIn, c.speed);
    auto limit = e.sourceLimit(c);
    if (!limit) return kInfinite;
    const Time room = *limit - c.sourceOut();
    return room.ticks > 0 ? inverseScale(room, c.speed) : Time{0};
}

// Removes (delta > 0) or reveals (delta < 0) content at the clip head without
// moving its start.
Status headTrimContent(const SequenceEditor& e, Clip& c, Time delta) {
    if (delta >= c.duration) return Status::error("Trim would make the clip empty");
    if (delta.ticks < 0 && -delta > maxHeadExtension(e, c)) return Status::error("Not enough source media");
    if (!c.freezeFrame && !c.reverse) {
        c.sourceIn = c.sourceIn + delta.scaled(c.speed, Rounding::Nearest);
        if (c.sourceIn.ticks < 0) c.sourceIn = Time{0};
    }
    c.duration = c.duration - delta;
    shiftClipKeys(c, -delta);
    return Status::ok();
}

Status headTrimValue(const SequenceEditor& e, Clip& c, Time newStart) {
    if (newStart.ticks < 0) return Status::error("Clip cannot start before 0");
    if (auto st = headTrimContent(e, c, newStart - c.start); !st) return st;
    c.start = newStart;
    return Status::ok();
}

Status tailTrimValue(const SequenceEditor& e, Clip& c, Time newEnd) {
    if (newEnd <= c.start) return Status::error("Trim would make the clip empty");
    const Time delta = newEnd - c.end();
    if (delta.ticks > 0 && delta > maxTailExtension(e, c)) return Status::error("Not enough source media");
    if (!c.freezeFrame && c.reverse) {
        c.sourceIn = c.sourceIn - delta.scaled(c.speed, Rounding::Nearest);
        if (c.sourceIn.ticks < 0) c.sourceIn = Time{0};
    }
    c.duration = newEnd - c.start;
    return Status::ok();
}

bool trackLocked(const SequenceEditor& e, int ti) { return e.trackAt(ti).locked; }

Time minClipDuration(const SequenceEditor& e) { return e.seq().frameDuration(); }

// Gap available to the left of the first clip starting at or after `from`.
Time leftRoomOnTrack(const Track& t, Time from) {
    const size_t n = t.clips.size();
    size_t i = 0;
    while (i < n && t.clips[i]->start < from) ++i;
    if (i == n) return kInfinite;  // nothing to shift
    const Time prevEnd = i > 0 ? t.clips[i - 1]->end() : Time{0};
    return t.clips[i]->start - prevEnd;
}

std::vector<int> familyIndices(const Sequence& seq, bool audio) {
    return audio ? seq.audioTrackIndices() : seq.visualTrackIndices();
}

int positionInFamily(const std::vector<int>& fam, int trackIndex) {
    for (size_t i = 0; i < fam.size(); ++i)
        if (fam[i] == trackIndex) return static_cast<int>(i);
    return -1;
}

bool mediaHas(const SequenceEditor& e, const Clip& c, bool& hasVideo, bool& hasAudio) {
    hasVideo = hasAudio = true;
    if (c.kind != ClipKind::Media) return true;
    const MediaItem* m = e.media(c.media);
    if (!m) return false;
    hasVideo = m->info.hasVideo();
    hasAudio = m->info.hasAudio();
    return true;
}

Status checkAccepts(const SequenceEditor& e, int ti, const Clip& c) {
    if (ti < 0 || ti >= e.trackCount()) return Status::error("Invalid track");
    const Track& t = e.trackAt(ti);
    if (t.locked) return Status::error("Track '" + t.name + "' is locked");
    bool hv, ha;
    if (!mediaHas(e, c, hv, ha)) return Status::error("Clip references missing media");
    if (!trackAccepts(t.kind, c.kind, hv, ha)) return Status::error("Track '" + t.name + "' cannot hold this clip");
    return Status::ok();
}

}  // namespace

// ============================================================ primitives

bool rangeIsEmpty(const Track& t, TimeRange range, const std::set<ClipId>& ignore) {
    for (size_t i = t.lowerBound(range.start); i < t.clips.size(); ++i) {
        const Clip& c = *t.clips[i];
        if (c.start >= range.end()) break;
        if (ignore.count(c.id)) continue;
        if (c.range().overlaps(range)) return false;
    }
    return true;
}

Status trimHeadTo(SequenceEditor& e, ClipId id, Time newStart) {
    const Clip* c = e.clip(id);
    if (!c) return Status::error("Clip not found");
    Clip v = *c;
    if (auto st = headTrimValue(e, v, newStart); !st) return st;
    e.mutableClip(id) = std::move(v);
    return Status::ok();
}

Status trimTailTo(SequenceEditor& e, ClipId id, Time newEnd) {
    const Clip* c = e.clip(id);
    if (!c) return Status::error("Clip not found");
    Clip v = *c;
    if (auto st = tailTrimValue(e, v, newEnd); !st) return st;
    e.mutableClip(id) = std::move(v);
    return Status::ok();
}

Status clearRange(SequenceEditor& e, int ti, TimeRange range) {
    if (range.empty()) return Status::ok();
    const Track& t = e.trackAt(ti);
    std::vector<ClipId> hits;
    for (size_t i = t.lowerBound(range.start); i < t.clips.size(); ++i) {
        const Clip& c = *t.clips[i];
        if (c.start >= range.end()) break;
        if (c.range().overlaps(range)) hits.push_back(c.id);
    }
    for (ClipId id : hits) {
        const Clip c = *e.clip(id);
        const bool startsInside = c.start >= range.start;
        const bool endsInside = c.end() <= range.end();
        if (startsInside && endsInside) {
            e.removeClip(id);
        } else if (!startsInside && !endsInside) {
            Clip right = c;
            right.id = newId();
            if (c.linkGroup != kInvalidId) right.linkGroup = deriveId(c.linkGroup, range.end().ticks);
            right.transitionIn.reset();
            if (auto st = headTrimValue(e, right, range.end()); !st) return st;
            if (auto st = trimTailTo(e, id, range.start); !st) return st;
            e.mutableClip(id).transitionOut.reset();
            if (!e.insertClip(ti, std::move(right))) return Status::error("Internal error splitting clip");
        } else if (!startsInside) {
            if (auto st = trimTailTo(e, id, range.start); !st) return st;
        } else {
            if (auto st = trimHeadTo(e, id, range.end()); !st) return st;
        }
    }
    return Status::ok();
}

Status shiftClips(SequenceEditor& e, int ti, Time from, Time delta) {
    if (delta.ticks == 0) return Status::ok();
    const Track& ro = e.trackAt(ti);
    bool any = false;
    for (auto& c : ro.clips)
        if (c->start >= from) {
            any = true;
            break;
        }
    if (!any) return Status::ok();
    Track& t = e.mutableTrack(ti);
    // Validate room when shifting left.
    if (delta.ticks < 0 && leftRoomOnTrack(t, from) < -delta) return Status::error("Not enough room to shift clips");
    for (size_t i = 0; i < t.clips.size(); ++i) {
        if (t.clips[i]->start >= from) {
            Clip& c = e.mutableClipAt(ti, static_cast<int>(i));
            c.start += delta;
        }
    }
    return Status::ok();
}

Status insertGap(SequenceEditor& e, int ti, Time at, Time duration) {
    if (duration.ticks <= 0) return Status::ok();
    const Track& t = e.trackAt(ti);
    const int idx = t.clipIndexAt(at);
    if (idx >= 0) {
        const Clip& c = *t.clips[static_cast<size_t>(idx)];
        if (c.start < at) {
            auto r = splitClip(e, c.id, at, false);
            if (!r) return r.status();
        }
    }
    return shiftClips(e, ti, at, duration);
}

std::vector<int> rippleTracks(const Sequence& seq, const std::vector<int>& tracks) {
    std::set<int> s;
    for (int t : tracks)
        if (t >= 0 && t < static_cast<int>(seq.tracks.size()) && !seq.tracks[static_cast<size_t>(t)]->locked) s.insert(t);
    for (size_t i = 0; i < seq.tracks.size(); ++i)
        if (seq.tracks[i]->syncLock && !seq.tracks[i]->locked) s.insert(static_cast<int>(i));
    return {s.begin(), s.end()};
}

std::set<ClipId> expandSelection(const Sequence& seq, const std::set<ClipId>& ids, bool linked, bool groups) {
    std::set<ClipId> out = ids;
    bool grew = true;
    while (grew) {
        grew = false;
        std::set<ClipId> add;
        for (ClipId id : out) {
            if (linked)
                for (ClipId l : seq.linkedClips(id)) add.insert(l);
            if (groups)
                for (ClipId g : seq.groupedClips(id)) add.insert(g);
        }
        for (ClipId a : add)
            if (out.insert(a).second) grew = true;
    }
    return out;
}

// ============================================================ placement

Status placeClips(SequenceEditor& e, std::vector<Placement> placements, PlaceMode mode) {
    if (placements.empty()) return Status::ok();
    Time at = Time::max(), endAt{0};
    std::vector<int> targets;
    for (auto& p : placements) {
        if (auto st = checkAccepts(e, p.trackIndex, p.clip); !st) return st;
        if (p.clip.duration.ticks <= 0) return Status::error("Clip has no duration");
        if (p.clip.start.ticks < 0) return Status::error("Clip cannot start before 0");
        if (p.clip.id == kInvalidId) p.clip.id = newId();
        at = minTime(at, p.clip.start);
        endAt = maxTime(endAt, p.clip.end());
        targets.push_back(p.trackIndex);
    }
    if (mode == PlaceMode::Insert) {
        for (int ti : rippleTracks(e.seq(), targets))
            if (auto st = insertGap(e, ti, at, endAt - at); !st) return st;
    }
    for (auto& p : placements) {
        if (auto st = clearRange(e, p.trackIndex, p.clip.range()); !st) return st;
        if (!e.insertClip(p.trackIndex, std::move(p.clip))) return Status::error("Could not place clip");
    }
    return Status::ok();
}

Result<std::vector<ClipId>> addMediaClip(SequenceEditor& e, const MediaItem& media, Time at, int videoTrack,
                                         int audioTrack, PlaceMode mode, std::optional<TimeRange> sourceRange) {
    const Rational fps = e.seq().frameRate;
    at = at.snappedToFrame(fps, Rounding::Nearest);
    if (at.ticks < 0) at = Time{0};
    Time sourceIn{0};
    Time duration;
    if (media.info.kind == MediaKind::Image) {
        duration = sourceRange ? sourceRange->duration : defaultStillDuration();
    } else {
        sourceIn = sourceRange ? sourceRange->start : Time{0};
        duration = sourceRange ? sourceRange->duration : media.info.duration - sourceIn;
    }
    // Frame-align the timeline duration without exceeding the source.
    const Time frame = Time::frameDuration(fps);
    Time aligned = Time::fromFrames(duration.toFrames(fps, Rounding::Down), fps);
    if (aligned.ticks <= 0) aligned = frame;
    duration = aligned;
    if (media.info.kind != MediaKind::Image && media.info.duration.ticks > 0 && sourceIn + duration > media.info.duration)
        duration = media.info.duration - sourceIn;
    if (duration.ticks <= 0) return Status::error("Media has no duration");

    std::vector<Placement> ps;
    const Id link = (media.info.hasVideo() && media.info.hasAudio() && videoTrack >= 0 && audioTrack >= 0)
                        ? newId()
                        : kInvalidId;
    if (media.info.hasVideo() && videoTrack >= 0) {
        Clip c = makeMediaClip(media, e.trackAt(videoTrack).kind, at, sourceIn, duration);
        c.linkGroup = link;
        ps.push_back({videoTrack, std::move(c)});
    }
    if (media.info.hasAudio() && audioTrack >= 0) {
        Clip c = makeMediaClip(media, TrackKind::Audio, at, sourceIn, duration);
        c.linkGroup = link;
        ps.push_back({audioTrack, std::move(c)});
    }
    if (ps.empty()) return Status::error("No compatible track for this media");
    std::vector<ClipId> ids;
    for (auto& p : ps) ids.push_back(p.clip.id);
    if (auto st = placeClips(e, std::move(ps), mode); !st) return st;
    return ids;
}

// ============================================================ split

Result<std::vector<ClipId>> splitClip(SequenceEditor& e, ClipId id, Time t, bool includeLinked) {
    const Clip* primary = e.clip(id);
    if (!primary) return Status::error("Clip not found");
    if (!(primary->start < t && t < primary->end())) return Status::error("Split point is outside the clip");
    std::vector<ClipId> ids = includeLinked ? e.seq().linkedClips(id) : std::vector<ClipId>{id};
    std::vector<ClipId> created;
    for (ClipId cid : ids) {
        auto ref = e.seq().findClip(cid);
        if (!ref) continue;
        if (e.trackAt(ref.track).locked) continue;
        const Clip c = *e.clip(cid);
        if (!(c.start < t && t < c.end())) continue;
        Clip right = c;
        right.id = newId();
        if (c.linkGroup != kInvalidId) right.linkGroup = deriveId(c.linkGroup, t.ticks);
        right.transitionIn.reset();
        if (auto st = headTrimValue(e, right, t); !st) return st;
        if (auto st = trimTailTo(e, cid, t); !st) return st;
        e.mutableClip(cid).transitionOut.reset();
        const ClipId rid = right.id;
        if (!e.insertClip(ref.track, std::move(right))) return Status::error("Internal error while splitting");
        created.push_back(rid);
    }
    return created;
}

Result<std::vector<ClipId>> splitAtTime(SequenceEditor& e, Time t, const std::vector<int>& tracks) {
    std::vector<int> list = tracks;
    if (list.empty())
        for (int i = 0; i < e.trackCount(); ++i) list.push_back(i);
    std::vector<ClipId> created;
    for (int ti : list) {
        if (ti < 0 || ti >= e.trackCount() || trackLocked(e, ti)) continue;
        const Track& tr = e.trackAt(ti);
        const int idx = tr.clipIndexAt(t);
        if (idx < 0) continue;
        const Clip& c = *tr.clips[static_cast<size_t>(idx)];
        if (c.start == t) continue;
        auto r = splitClip(e, c.id, t, false);
        if (!r) return r;
        created.insert(created.end(), r->begin(), r->end());
    }
    return created;
}

// ============================================================ trim

Status trimClip(SequenceEditor& e, ClipId id, Edge edge, Time newTime, bool ripple, bool includeLinked) {
    const Clip* primary = e.clip(id);
    if (!primary) return Status::error("Clip not found");
    const std::vector<ClipId> ids = includeLinked ? e.seq().linkedClips(id) : std::vector<ClipId>{id};
    const Time oldEdge = edge == Edge::Head ? primary->start : primary->end();
    Time delta = newTime - oldEdge;
    if (delta.ticks == 0) return Status::ok();

    // Compute the allowed delta range across the clip and its partners.
    Time lo = -Time::max(), hi = Time::max();
    std::vector<int> ownTracks;
    for (ClipId cid : ids) {
        auto ref = e.seq().findClip(cid);
        if (!ref || trackLocked(e, ref.track)) continue;
        ownTracks.push_back(ref.track);
        const Clip& c = *e.clip(cid);
        const Track& t = e.trackAt(ref.track);
        const Time minDur = minTime(minClipDuration(e), c.duration);
        if (edge == Edge::Head) {
            hi = minTime(hi, c.duration - minDur);            // trim in
            lo = maxTime(lo, -maxHeadExtension(e, c));        // extend out
            if (!ripple) {
                const Time prevEnd = ref.clip > 0 ? t.clips[static_cast<size_t>(ref.clip - 1)]->end() : Time{0};
                lo = maxTime(lo, prevEnd - c.start);
            } else {
                // Ripple head extension moves the clip content later; start stays.
            }
            if (!ripple) lo = maxTime(lo, -c.start);
        } else {
            lo = maxTime(lo, -(c.duration - minDur));
            hi = minTime(hi, maxTailExtension(e, c));
            if (!ripple) {
                const Time nextStart =
                    static_cast<size_t>(ref.clip + 1) < t.clips.size() ? t.clips[static_cast<size_t>(ref.clip + 1)]->start : Time::max();
                hi = minTime(hi, nextStart - c.end());
            }
        }
    }
    if (ownTracks.empty()) return Status::error("Clip is on a locked track");

    std::vector<int> shiftTracks;
    if (ripple) {
        shiftTracks = rippleTracks(e.seq(), ownTracks);
        // Shifting left (tail trim-in, head trim-in) needs room on other tracks.
        const Time shiftFrom = primary->end();
        for (int ti : shiftTracks) {
            if (std::find(ownTracks.begin(), ownTracks.end(), ti) != ownTracks.end()) continue;
            const Time room = leftRoomOnTrack(e.trackAt(ti), shiftFrom);
            if (edge == Edge::Tail) lo = maxTime(lo, -room);
            else hi = minTime(hi, room);
        }
    }
    delta = std::clamp(delta, lo, hi);
    if (delta.ticks == 0) return Status::ok();

    if (!ripple) {
        for (ClipId cid : ids) {
            auto ref = e.seq().findClip(cid);
            if (!ref || trackLocked(e, ref.track)) continue;
            const Clip& c = *e.clip(cid);
            Status st = edge == Edge::Head ? trimHeadTo(e, cid, c.start + delta) : trimTailTo(e, cid, c.end() + delta);
            if (!st) return st;
        }
        return Status::ok();
    }

    // Ripple: the shift applied to later material.
    const Time shift = edge == Edge::Tail ? delta : -delta;
    const Time oldEnd = primary->end();
    // Shift later clips first when growing (make room), after trimming when shrinking.
    auto doShift = [&]() -> Status {
        for (int ti : shiftTracks) {
            if (auto st = shiftClips(e, ti, oldEnd, shift); !st) return st;
        }
        return Status::ok();
    };
    if (shift.ticks > 0)
        if (auto st = doShift(); !st) return st;
    for (ClipId cid : ids) {
        auto ref = e.seq().findClip(cid);
        if (!ref || trackLocked(e, ref.track)) continue;
        Clip v = *e.clip(cid);
        if (edge == Edge::Tail) {
            if (auto st = tailTrimValue(e, v, v.end() + delta); !st) return st;
        } else {
            // Content trimmed at the head; the clip keeps its start position.
            if (auto st = headTrimContent(e, v, delta); !st) return st;
        }
        e.mutableClip(cid) = std::move(v);
    }
    if (shift.ticks < 0)
        if (auto st = doShift(); !st) return st;
    return Status::ok();
}

Status rollEdit(SequenceEditor& e, ClipId leftId, ClipId rightId, Time newCut) {
    const Clip* l = e.clip(leftId);
    const Clip* r = e.clip(rightId);
    if (!l || !r) return Status::error("Clip not found");
    auto lref = e.seq().findClip(leftId), rref = e.seq().findClip(rightId);
    if (lref.track != rref.track || l->end() != r->start) return Status::error("Clips are not adjacent");
    if (trackLocked(e, lref.track)) return Status::error("Track is locked");

    // Collect partner edit points (linked clips forming the same cut).
    struct Pair {
        ClipId left, right;
    };
    std::vector<Pair> pairs{{leftId, rightId}};
    const Time cut = l->end();
    if (l->linkGroup != kInvalidId) {
        for (ClipId pl : e.seq().linkedClips(leftId)) {
            if (pl == leftId) continue;
            auto ref = e.seq().findClip(pl);
            const Track& t = e.trackAt(ref.track);
            if (t.locked || static_cast<size_t>(ref.clip + 1) >= t.clips.size()) continue;
            const Clip& a = *t.clips[static_cast<size_t>(ref.clip)];
            const Clip& b = *t.clips[static_cast<size_t>(ref.clip + 1)];
            if (a.end() == cut && b.start == cut) pairs.push_back({a.id, b.id});
        }
    }
    Time delta = newCut - cut;
    Time lo = -Time::max(), hi = Time::max();
    for (auto& p : pairs) {
        const Clip& a = *e.clip(p.left);
        const Clip& b = *e.clip(p.right);
        const Time minA = minTime(minClipDuration(e), a.duration), minB = minTime(minClipDuration(e), b.duration);
        hi = minTime(hi, minTime(maxTailExtension(e, a), b.duration - minB));
        lo = maxTime(lo, maxTime(-maxHeadExtension(e, b), -(a.duration - minA)));
    }
    delta = std::clamp(delta, lo, hi);
    if (delta.ticks == 0) return Status::ok();
    for (auto& p : pairs) {
        const Time c2 = cut + delta;
        if (delta.ticks > 0) {
            if (auto st = trimHeadTo(e, p.right, c2); !st) return st;
            if (auto st = trimTailTo(e, p.left, c2); !st) return st;
        } else {
            if (auto st = trimTailTo(e, p.left, c2); !st) return st;
            if (auto st = trimHeadTo(e, p.right, c2); !st) return st;
        }
    }
    return Status::ok();
}

Status slipClip(SequenceEditor& e, ClipId id, Time sourceDelta, bool includeLinked) {
    const std::vector<ClipId> ids = includeLinked ? e.seq().linkedClips(id) : std::vector<ClipId>{id};
    Time lo = -Time::max(), hi = Time::max();
    for (ClipId cid : ids) {
        const Clip* c = e.clip(cid);
        if (!c) continue;
        if (!c->hasLimitedSource()) continue;
        lo = maxTime(lo, -c->sourceIn);
        if (auto limit = e.sourceLimit(*c)) hi = minTime(hi, *limit - c->sourceOut());
    }
    const Time d = std::clamp(sourceDelta, lo, maxTime(lo, hi));
    if (d.ticks == 0) return Status::ok();
    for (ClipId cid : ids) {
        auto ref = e.seq().findClip(cid);
        if (!ref || trackLocked(e, ref.track)) continue;
        Clip& c = e.mutableClip(cid);
        if (!c.hasLimitedSource()) continue;
        c.sourceIn += d;
    }
    return Status::ok();
}

Status slideClip(SequenceEditor& e, ClipId id, Time delta) {
    auto ref = e.seq().findClip(id);
    if (!ref) return Status::error("Clip not found");
    if (trackLocked(e, ref.track)) return Status::error("Track is locked");
    const Track& t = e.trackAt(ref.track);
    const Clip& c = *t.clips[static_cast<size_t>(ref.clip)];
    const Clip* prev = ref.clip > 0 ? t.clips[static_cast<size_t>(ref.clip - 1)].get() : nullptr;
    const Clip* next = static_cast<size_t>(ref.clip + 1) < t.clips.size() ? t.clips[static_cast<size_t>(ref.clip + 1)].get() : nullptr;
    const bool prevAdj = prev && prev->end() == c.start;
    const bool nextAdj = next && next->start == c.end();
    Time lo = -c.start, hi = Time::max();
    if (prevAdj) {
        hi = minTime(hi, maxTailExtension(e, *prev));
        lo = maxTime(lo, -(prev->duration - minTime(minClipDuration(e), prev->duration)));
    } else if (prev) {
        lo = maxTime(lo, prev->end() - c.start);
    }
    if (nextAdj) {
        hi = minTime(hi, next->duration - minTime(minClipDuration(e), next->duration));
        lo = maxTime(lo, -maxHeadExtension(e, *next));
    } else if (next) {
        hi = minTime(hi, next->start - c.end());
    }
    delta = std::clamp(delta, lo, maxTime(lo, hi));
    if (delta.ticks == 0) return Status::ok();
    const ClipId prevId = prevAdj ? prev->id : kInvalidId;
    const ClipId nextId = nextAdj ? next->id : kInvalidId;
    const Time newStart = c.start + delta, newEnd = c.end() + delta;
    if (delta.ticks > 0) {
        if (nextId) if (auto st = trimHeadTo(e, nextId, newEnd); !st) return st;
        e.mutableClip(id).start = newStart;
        if (prevId) if (auto st = trimTailTo(e, prevId, newStart); !st) return st;
    } else {
        if (prevId) if (auto st = trimTailTo(e, prevId, newStart); !st) return st;
        e.mutableClip(id).start = newStart;
        if (nextId) if (auto st = trimHeadTo(e, nextId, newEnd); !st) return st;
    }
    return Status::ok();
}

// ============================================================ move / delete

Status moveClips(SequenceEditor& e, const std::set<ClipId>& ids, Time delta, int vDelta, int aDelta, PlaceMode mode) {
    if (ids.empty()) return Status::ok();
    const Sequence& seq = e.seq();
    const auto vis = familyIndices(seq, false), aud = familyIndices(seq, true);
    std::vector<Placement> moves;
    Time minStart = Time::max();
    for (ClipId id : ids) {
        auto ref = seq.findClip(id);
        if (!ref) return Status::error("Clip not found");
        if (trackLocked(e, ref.track)) return Status::error("Clip is on a locked track");
        const bool audio = !isVisualTrack(seq.tracks[static_cast<size_t>(ref.track)]->kind);
        const auto& fam = audio ? aud : vis;
        const int pos = positionInFamily(fam, ref.track) + (audio ? aDelta : vDelta);
        if (pos < 0 || pos >= static_cast<int>(fam.size())) return Status::error("No track at destination");
        Placement p{fam[static_cast<size_t>(pos)], *seq.clip(id)};
        minStart = minTime(minStart, p.clip.start);
        moves.push_back(std::move(p));
    }
    if (minStart + delta < Time{0}) delta = -minStart;
    for (auto& m : moves) {
        m.clip.start += delta;
        if (auto st = checkAccepts(e, m.trackIndex, m.clip); !st) return st;
    }
    for (ClipId id : ids) e.removeClip(id);
    return placeClips(e, std::move(moves), mode);
}

Status deleteClips(SequenceEditor& e, const std::set<ClipId>& ids) {
    for (ClipId id : ids) {
        auto ref = e.seq().findClip(id);
        if (!ref) continue;
        if (trackLocked(e, ref.track)) return Status::error("Clip is on a locked track");
        e.removeClip(id);
    }
    return Status::ok();
}

Status rippleDelete(SequenceEditor& e, const std::set<ClipId>& ids) {
    // Merge selected clip ranges.
    std::vector<TimeRange> ranges;
    std::set<int> selTracks;
    for (ClipId id : ids) {
        auto ref = e.seq().findClip(id);
        if (!ref) continue;
        if (trackLocked(e, ref.track)) return Status::error("Clip is on a locked track");
        ranges.push_back(e.clip(id)->range());
        selTracks.insert(ref.track);
    }
    if (ranges.empty()) return Status::ok();
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (auto& r : ranges) {
        if (!merged.empty() && r.start <= merged.back().end())
            merged.back() = TimeRange::fromStartEnd(merged.back().start, maxTime(merged.back().end(), r.end()));
        else
            merged.push_back(r);
    }
    if (auto st = deleteClips(e, ids); !st) return st;
    const auto tracks = rippleTracks(e.seq(), {selTracks.begin(), selTracks.end()});
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
        for (int ti : tracks) {
            const Track& t = e.trackAt(ti);
            if (!rangeIsEmpty(t, *it)) continue;  // content on this track keeps its position
            // Only close as much gap as is actually free before the next clip.
            const Time room = leftRoomOnTrack(t, it->end());
            const Time shift = minTime(it->duration, room);
            if (shift.ticks > 0)
                if (auto st = shiftClips(e, ti, it->end(), -shift); !st) return st;
        }
    }
    return Status::ok();
}

Status lift(SequenceEditor& e, TimeRange range, const std::vector<int>& tracks) {
    for (int ti : tracks) {
        if (ti < 0 || ti >= e.trackCount() || trackLocked(e, ti)) continue;
        if (auto st = clearRange(e, ti, range); !st) return st;
    }
    return Status::ok();
}

Status extract(SequenceEditor& e, TimeRange range, const std::vector<int>& tracks) {
    if (auto st = lift(e, range, tracks); !st) return st;
    for (int ti : rippleTracks(e.seq(), tracks)) {
        const Track& t = e.trackAt(ti);
        if (!rangeIsEmpty(t, range)) continue;
        const Time shift = minTime(range.duration, leftRoomOnTrack(t, range.end()));
        if (shift.ticks > 0)
            if (auto st = shiftClips(e, ti, range.end(), -shift); !st) return st;
    }
    return Status::ok();
}

Status closeGap(SequenceEditor& e, int ti, Time t) {
    if (ti < 0 || ti >= e.trackCount()) return Status::error("Invalid track");
    const Track& tr = e.trackAt(ti);
    if (tr.clipIndexAt(t) >= 0) return Status::error("No gap at this position");
    const size_t next = tr.lowerBound(t);
    if (next >= tr.clips.size()) return Status::error("No clip after the gap");
    const Time gapEnd = tr.clips[next]->start;
    const Time gapStart = next > 0 ? tr.clips[next - 1]->end() : Time{0};
    const TimeRange gap = TimeRange::fromStartEnd(gapStart, gapEnd);
    for (int r : rippleTracks(e.seq(), {ti})) {
        const Track& rt = e.trackAt(r);
        if (!rangeIsEmpty(rt, gap)) continue;
        const Time shift = minTime(gap.duration, leftRoomOnTrack(rt, gapEnd));
        if (shift.ticks > 0)
            if (auto st = shiftClips(e, r, gapEnd, -shift); !st) return st;
    }
    return Status::ok();
}

Result<std::vector<ClipId>> duplicateClips(SequenceEditor& e, const std::set<ClipId>& ids) {
    if (ids.empty()) return std::vector<ClipId>{};
    Time minStart = Time::max(), maxEnd{0};
    for (ClipId id : ids) {
        const Clip* c = e.clip(id);
        if (!c) return Status::error("Clip not found");
        minStart = minTime(minStart, c->start);
        maxEnd = maxTime(maxEnd, c->end());
    }
    const Time offset = maxEnd - minStart;
    std::map<Id, Id> linkMap, groupMap;
    std::vector<Placement> ps;
    std::vector<ClipId> created;
    for (ClipId id : ids) {
        auto ref = e.seq().findClip(id);
        Clip c = *e.clip(id);
        c.id = newId();
        c.start += offset;
        if (c.linkGroup) {
            auto [it, ins] = linkMap.emplace(c.linkGroup, 0);
            if (ins) it->second = newId();
            c.linkGroup = it->second;
        }
        if (c.group) {
            auto [it, ins] = groupMap.emplace(c.group, 0);
            if (ins) it->second = newId();
            c.group = it->second;
        }
        created.push_back(c.id);
        ps.push_back({ref.track, std::move(c)});
    }
    if (auto st = placeClips(e, std::move(ps), PlaceMode::Overwrite); !st) return st;
    return created;
}

Status replaceClipMedia(SequenceEditor& e, ClipId id, const MediaItem& media, Time sourceIn) {
    auto ref = e.seq().findClip(id);
    if (!ref) return Status::error("Clip not found");
    Clip c = *e.clip(id);
    if (c.kind != ClipKind::Media) return Status::error("Only media clips can be replaced");
    const Track& t = e.trackAt(ref.track);
    if (!trackAccepts(t.kind, ClipKind::Media, media.info.hasVideo(), media.info.hasAudio()))
        return Status::error("Media is not compatible with this track");
    c.media = media.id;
    c.name = media.name;
    c.sourceIn = sourceIn;
    c.streamIndex = -1;
    if (media.info.kind != MediaKind::Image && media.info.duration.ticks > 0) {
        if (c.sourceIn >= media.info.duration) c.sourceIn = Time{0};
        const Time avail = inverseScale(media.info.duration - c.sourceIn, c.speed);
        if (c.duration > avail) c.duration = avail;
    }
    if (c.duration.ticks <= 0) return Status::error("Media too short");
    e.mutableClip(id) = std::move(c);
    return Status::ok();
}

Status setClipsEnabled(SequenceEditor& e, const std::set<ClipId>& ids, bool enabled) {
    for (ClipId id : ids)
        if (e.clip(id)) e.mutableClip(id).enabled = enabled;
    return Status::ok();
}

Status groupClips(SequenceEditor& e, const std::set<ClipId>& ids) {
    if (ids.size() < 2) return Status::error("Select at least two clips to group");
    const Id g = newId();
    for (ClipId id : ids)
        if (e.clip(id)) e.mutableClip(id).group = g;
    return Status::ok();
}

Status ungroupClips(SequenceEditor& e, const std::set<ClipId>& ids) {
    for (ClipId id : ids)
        if (e.clip(id)) e.mutableClip(id).group = kInvalidId;
    return Status::ok();
}

Status linkClips(SequenceEditor& e, const std::set<ClipId>& ids) {
    if (ids.size() < 2) return Status::error("Select at least two clips to link");
    const Id g = newId();
    for (ClipId id : ids)
        if (e.clip(id)) e.mutableClip(id).linkGroup = g;
    return Status::ok();
}

Status unlinkClips(SequenceEditor& e, const std::set<ClipId>& ids) {
    for (ClipId id : ids)
        if (e.clip(id)) e.mutableClip(id).linkGroup = kInvalidId;
    return Status::ok();
}

Status setClipSpeed(SequenceEditor& e, ClipId id, Rational speed, bool ripple) {
    speed = speed.reduced();
    if (speed.num <= 0 || speed.den <= 0) return Status::error("Speed must be positive");
    auto ref = e.seq().findClip(id);
    if (!ref) return Status::error("Clip not found");
    if (trackLocked(e, ref.track)) return Status::error("Track is locked");
    const Clip& c = *e.clip(id);
    const Rational fps = e.seq().frameRate;
    const Time srcDur = c.freezeFrame ? c.duration : c.sourceDuration();
    Time newDur = srcDur.scaled(speed.inverse(), Rounding::Nearest);
    newDur = maxTime(Time::frameDuration(fps), Time::fromFrames(newDur.toFrames(fps, Rounding::Nearest), fps));
    const Time oldEnd = c.end();
    const Time growth = newDur - c.duration;
    if (!ripple && growth.ticks > 0) {
        const Track& t = e.trackAt(ref.track);
        const Time nextStart = static_cast<size_t>(ref.clip + 1) < t.clips.size() ? t.clips[static_cast<size_t>(ref.clip + 1)]->start : Time::max();
        if (c.start + newDur > nextStart) newDur = nextStart - c.start;
    }
    // Keep within source.
    if (auto limit = e.sourceLimit(c); limit && !c.freezeFrame) {
        const Time maxDur = inverseScale(*limit - c.sourceIn, speed);
        if (newDur > maxDur) newDur = maxDur;
    }
    if (newDur.ticks <= 0) return Status::error("Resulting clip would be empty");
    const Time shift = newDur - c.duration;
    if (ripple && shift.ticks > 0)
        for (int ti : rippleTracks(e.seq(), {ref.track}))
            if (auto st = shiftClips(e, ti, oldEnd, shift); !st) return st;
    Clip& m = e.mutableClip(id);
    m.speed = speed;
    m.duration = newDur;
    if (ripple && shift.ticks < 0) {
        for (int ti : rippleTracks(e.seq(), {ref.track})) {
            const Time room = leftRoomOnTrack(e.trackAt(ti), oldEnd);
            const Time s = maxTime(shift, -room);
            if (auto st = shiftClips(e, ti, oldEnd, s); !st) return st;
        }
    }
    return Status::ok();
}

Status setClipReverse(SequenceEditor& e, ClipId id, bool reverse) {
    if (!e.clip(id)) return Status::error("Clip not found");
    e.mutableClip(id).reverse = reverse;
    return Status::ok();
}

Result<ClipId> insertFreezeFrame(SequenceEditor& e, ClipId id, Time t, Time duration) {
    auto ref = e.seq().findClip(id);
    if (!ref) return Status::error("Clip not found");
    const Clip src = *e.clip(id);
    if (!src.range().contains(t)) return Status::error("Playhead is not over the clip");
    const Time srcTime = src.sourceTimeAt(t);
    for (int ti : rippleTracks(e.seq(), {ref.track}))
        if (auto st = insertGap(e, ti, t, duration); !st) return st;
    Clip f = src;
    f.id = newId();
    f.start = t;
    f.duration = duration;
    f.sourceIn = srcTime;
    f.freezeFrame = true;
    f.linkGroup = kInvalidId;
    f.transitionIn.reset();
    f.transitionOut.reset();
    f.name = src.name + " (Freeze)";
    shiftClipKeys(f, src.start - t);
    const ClipId fid = f.id;
    if (!e.insertClip(ref.track, std::move(f))) return Status::error("Could not insert freeze frame");
    return fid;
}

// ============================================================ clipboard

Clipboard copyClips(const Sequence& seq, const std::set<ClipId>& ids) {
    Clipboard cb;
    Time origin = Time::max();
    int minVis = 1 << 30, minAud = 1 << 30;
    const auto vis = seq.visualTrackIndices(), aud = seq.audioTrackIndices();
    for (ClipId id : ids) {
        auto ref = seq.findClip(id);
        if (!ref) continue;
        origin = minTime(origin, seq.clip(id)->start);
        const bool audio = !isVisualTrack(seq.tracks[static_cast<size_t>(ref.track)]->kind);
        const int pos = positionInFamily(audio ? aud : vis, ref.track);
        if (audio) minAud = std::min(minAud, pos);
        else minVis = std::min(minVis, pos);
    }
    for (ClipId id : ids) {
        auto ref = seq.findClip(id);
        if (!ref) continue;
        const bool audio = !isVisualTrack(seq.tracks[static_cast<size_t>(ref.track)]->kind);
        ClipboardItem item;
        item.audioFamily = audio;
        item.trackOffset = positionInFamily(audio ? aud : vis, ref.track) - (audio ? minAud : minVis);
        item.clip = *seq.clip(id);
        item.clip.start -= origin;
        cb.items.push_back(std::move(item));
    }
    return cb;
}

Result<std::vector<ClipId>> pasteClips(SequenceEditor& e, const Clipboard& cb, Time at, int visualBaseTrack,
                                       int audioBaseTrack, PlaceMode mode) {
    if (cb.empty()) return std::vector<ClipId>{};
    // Resolve base positions within each family, then make sure enough tracks exist.
    const TrackId visBaseId = visualBaseTrack >= 0 && visualBaseTrack < e.trackCount() ? e.trackAt(visualBaseTrack).id : kInvalidId;
    const TrackId audBaseId = audioBaseTrack >= 0 && audioBaseTrack < e.trackCount() ? e.trackAt(audioBaseTrack).id : kInvalidId;
    auto basePos = [&](bool audio) {
        const auto fam = familyIndices(e.seq(), audio);
        const TrackId bid = audio ? audBaseId : visBaseId;
        for (size_t i = 0; i < fam.size(); ++i)
            if (e.trackAt(fam[i]).id == bid) return static_cast<int>(i);
        return 0;
    };
    for (bool audio : {false, true}) {
        int needed = -1;
        for (auto& item : cb.items)
            if (item.audioFamily == audio) needed = std::max(needed, basePos(audio) + item.trackOffset);
        while (needed >= static_cast<int>(familyIndices(e.seq(), audio).size())) {
            auto r = addTrack(e, audio ? TrackKind::Audio : TrackKind::Video);
            if (!r) return r.status();
        }
    }
    std::map<Id, Id> linkMap, groupMap;
    std::vector<Placement> ps;
    std::vector<ClipId> created;
    for (auto& item : cb.items) {
        const auto fam = familyIndices(e.seq(), item.audioFamily);
        const int pos = basePos(item.audioFamily) + item.trackOffset;
        Clip c = item.clip;
        c.id = newId();
        c.start = at + item.clip.start;
        if (c.linkGroup) {
            auto [it, ins] = linkMap.emplace(c.linkGroup, 0);
            if (ins) it->second = newId();
            c.linkGroup = it->second;
        }
        if (c.group) {
            auto [it, ins] = groupMap.emplace(c.group, 0);
            if (ins) it->second = newId();
            c.group = it->second;
        }
        created.push_back(c.id);
        ps.push_back({fam[static_cast<size_t>(pos)], std::move(c)});
    }
    if (auto st = placeClips(e, std::move(ps), mode); !st) return st;
    return created;
}

// ============================================================ tracks

Result<TrackId> addTrack(SequenceEditor& e, TrackKind kind, std::string name, int familyPosition) {
    Sequence& s = e.props();
    const bool audio = !isVisualTrack(kind);
    const auto fam = familyIndices(s, audio);
    int insertAt;
    if (audio) {
        const int firstAudio = fam.empty() ? static_cast<int>(s.tracks.size()) : fam.front();
        insertAt = (familyPosition < 0 || familyPosition >= static_cast<int>(fam.size()))
                       ? static_cast<int>(s.tracks.size())
                       : firstAudio + familyPosition;
    } else {
        const int afterVisual = fam.empty() ? 0 : fam.back() + 1;
        insertAt = (familyPosition < 0 || familyPosition >= static_cast<int>(fam.size()))
                       ? afterVisual
                       : fam[static_cast<size_t>(familyPosition)];
    }
    if (name.empty()) {
        const char* prefix = kind == TrackKind::Audio      ? "A"
                             : kind == TrackKind::Text     ? "T"
                             : kind == TrackKind::Subtitle ? "S"
                             : kind == TrackKind::Adjustment || kind == TrackKind::Effect ? "FX"
                                                                                        : "V";
        name = std::string(prefix) + std::to_string(s.countTracks(kind) + 1);
    }
    Track t = makeTrack(kind, std::move(name));
    const TrackId id = t.id;
    s.tracks.insert(s.tracks.begin() + insertAt, std::make_shared<Track>(std::move(t)));
    return id;
}

Status removeTrack(SequenceEditor& e, TrackId id) {
    Sequence& s = e.props();
    const int i = s.trackIndex(id);
    if (i < 0) return Status::error("Track not found");
    if (s.tracks[static_cast<size_t>(i)]->locked) return Status::error("Track is locked");
    s.tracks.erase(s.tracks.begin() + i);
    return Status::ok();
}

Status moveTrack(SequenceEditor& e, TrackId id, int newFamilyPosition) {
    Sequence& s = e.props();
    const int i = s.trackIndex(id);
    if (i < 0) return Status::error("Track not found");
    const bool audio = !isVisualTrack(s.tracks[static_cast<size_t>(i)]->kind);
    auto fam = familyIndices(s, audio);
    newFamilyPosition = std::clamp(newFamilyPosition, 0, static_cast<int>(fam.size()) - 1);
    const int target = fam[static_cast<size_t>(newFamilyPosition)];
    if (target == i) return Status::ok();
    TrackPtr t = s.tracks[static_cast<size_t>(i)];
    s.tracks.erase(s.tracks.begin() + i);
    s.tracks.insert(s.tracks.begin() + target, std::move(t));
    return Status::ok();
}

// ============================================================ markers

MarkerId addMarker(SequenceEditor& e, Marker m) {
    if (m.id == kInvalidId) m.id = newId();
    auto& v = e.props().markers;
    auto it = std::upper_bound(v.begin(), v.end(), m.time, [](Time t, const Marker& x) { return t < x.time; });
    const MarkerId id = m.id;
    v.insert(it, std::move(m));
    return id;
}

bool removeMarker(SequenceEditor& e, MarkerId id) {
    auto& v = e.props().markers;
    auto it = std::find_if(v.begin(), v.end(), [id](const Marker& m) { return m.id == id; });
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

bool updateMarker(SequenceEditor& e, const Marker& m) {
    if (!removeMarker(e, m.id)) return false;
    addMarker(e, m);
    return true;
}

void removeMarkersOfKind(SequenceEditor& e, MarkerKind kind) {
    auto& v = e.props().markers;
    v.erase(std::remove_if(v.begin(), v.end(), [kind](const Marker& m) { return m.kind == kind; }), v.end());
}

// ============================================================ compound

Result<SequenceId> createCompoundClip(ProjectEditor& pe, SequenceId seqId, const std::set<ClipId>& idsIn, std::string name) {
    const Sequence* src = pe.current().findSequence(seqId);
    if (!src) return Status::error("Sequence not found");
    const std::set<ClipId> ids = expandSelection(*src, idsIn);
    if (ids.empty()) return Status::error("Nothing selected");
    Time minStart = Time::max(), maxEnd{0};
    std::map<int, std::vector<Clip>> byTrack;
    for (ClipId id : ids) {
        auto ref = src->findClip(id);
        if (!ref) continue;
        if (src->tracks[static_cast<size_t>(ref.track)]->locked) return Status::error("Selection includes a locked track");
        const Clip& c = *src->clip(id);
        minStart = minTime(minStart, c.start);
        maxEnd = maxTime(maxEnd, c.end());
        byTrack[ref.track].push_back(c);
    }
    Sequence nested = makeSequence(name.empty() ? "Compound Clip" : name, src->width, src->height, src->frameRate, 0, 0);
    nested.sampleRate = src->sampleRate;
    int lowestVisual = -1, lowestAudio = -1;
    for (auto& [ti, clips] : byTrack) {  // std::map iterates in track order
        const Track& st = *src->tracks[static_cast<size_t>(ti)];
        Track t = makeTrack(st.kind, st.name);
        for (auto& c : clips) {
            Clip moved = c;
            moved.start -= minStart;
            t.clips.push_back(std::make_shared<Clip>(std::move(moved)));
        }
        std::sort(t.clips.begin(), t.clips.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
        nested.tracks.push_back(std::make_shared<Track>(std::move(t)));
        if (isVisualTrack(st.kind)) {
            if (lowestVisual < 0) lowestVisual = ti;
        } else if (lowestAudio < 0) {
            lowestAudio = ti;
        }
    }
    const SequenceId nestedId = pe.addSequence(std::move(nested));
    SequenceEditor e = pe.sequence(seqId);
    if (auto st = deleteClips(e, ids); !st) return st;
    const Id link = (lowestVisual >= 0 && lowestAudio >= 0) ? newId() : kInvalidId;
    auto makeCompound = [&](int ti) {
        Clip c;
        c.id = newId();
        c.kind = ClipKind::Compound;
        c.name = name.empty() ? "Compound Clip" : name;
        c.nested = nestedId;
        c.start = minStart;
        c.duration = maxEnd - minStart;
        c.sourceIn = Time{0};
        c.linkGroup = link;
        if (isVisualTrack(e.trackAt(ti).kind)) c.transform = defaultTransformParams();
        else c.audio = defaultAudioParams();
        return c;
    };
    if (lowestVisual >= 0) {
        if (e.trackAt(lowestVisual).kind != TrackKind::Video) return Status::error("Compound clips need a video track");
        if (!e.insertClip(lowestVisual, makeCompound(lowestVisual))) return Status::error("Could not place compound clip");
    }
    if (lowestAudio >= 0)
        if (!e.insertClip(lowestAudio, makeCompound(lowestAudio))) return Status::error("Could not place compound clip");
    return nestedId;
}

}  // namespace avc::edit
