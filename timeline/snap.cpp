#include "timeline/snap.h"

#include <algorithm>

namespace avc {

std::vector<SnapPoint> collectSnapPoints(const Sequence& seq, Time playhead, const std::set<ClipId>& exclude,
                                         const SnapOptions& opt, const std::set<ClipId>& keyframeClips) {
    std::vector<SnapPoint> pts;
    if (opt.clips || opt.transitions || opt.keyframes) {
        for (size_t ti = 0; ti < seq.tracks.size(); ++ti) {
            for (auto& cp : seq.tracks[ti]->clips) {
                const Clip& c = *cp;
                if (exclude.count(c.id)) continue;
                if (opt.clips) {
                    pts.push_back({c.start, SnapKind::ClipStart, c.id, static_cast<int>(ti)});
                    pts.push_back({c.end(), SnapKind::ClipEnd, c.id, static_cast<int>(ti)});
                }
                if (opt.transitions) {
                    if (c.transitionIn && c.transitionIn->duration.ticks > 0)
                        pts.push_back({c.start + c.transitionIn->duration, SnapKind::Transition, c.id, static_cast<int>(ti)});
                    if (c.transitionOut && c.transitionOut->duration.ticks > 0)
                        pts.push_back({c.end() - c.transitionOut->duration, SnapKind::Transition, c.id, static_cast<int>(ti)});
                }
                if (opt.keyframes && keyframeClips.count(c.id)) {
                    auto addKeys = [&](const ParamSet& ps) {
                        for (auto& [id, p] : ps.items())
                            for (auto& k : p.keys())
                                if (k.time.ticks >= 0 && k.time <= c.duration)
                                    pts.push_back({c.start + k.time, SnapKind::Keyframe, c.id, static_cast<int>(ti)});
                    };
                    addKeys(c.transform);
                    addKeys(c.audio);
                    addKeys(c.textParams);
                    for (auto& fx : c.effects) addKeys(fx.params);
                }
            }
        }
    }
    if (opt.playhead) pts.push_back({playhead, SnapKind::Playhead, kInvalidId, -1});
    for (auto& m : seq.markers) {
        if (m.kind == MarkerKind::Beat) {
            if (opt.beats) pts.push_back({m.time, SnapKind::Beat, m.id, -1});
        } else if (opt.markers) {
            pts.push_back({m.time, SnapKind::Marker, m.id, -1});
            if (m.duration.ticks > 0) pts.push_back({m.time + m.duration, SnapKind::Marker, m.id, -1});
        }
    }
    if (seq.workArea) {
        pts.push_back({seq.workArea->start, SnapKind::WorkArea, kInvalidId, -1});
        pts.push_back({seq.workArea->end(), SnapKind::WorkArea, kInvalidId, -1});
    }
    pts.push_back({Time{0}, SnapKind::WorkArea, kInvalidId, -1});
    std::sort(pts.begin(), pts.end(), [](const SnapPoint& a, const SnapPoint& b) { return a.time < b.time; });
    return pts;
}

SnapResult snapTime(const std::vector<SnapPoint>& points, Time t, Time threshold) {
    SnapResult r;
    r.time = t;
    if (points.empty() || threshold.ticks <= 0) return r;
    auto it = std::lower_bound(points.begin(), points.end(), t, [](const SnapPoint& p, Time v) { return p.time < v; });
    int64_t best = threshold.ticks + 1;
    auto consider = [&](std::vector<SnapPoint>::const_iterator c) {
        if (c < points.begin() || c >= points.end()) return;
        const int64_t d = std::llabs((c->time - t).ticks);
        // Prefer playhead / markers on ties: they are listed as distinct kinds.
        if (d < best || (d == best && c->kind == SnapKind::Playhead)) {
            best = d;
            r.snapped = true;
            r.time = c->time;
            r.point = *c;
        }
    };
    for (int k = -2; k <= 2; ++k) consider(it + k);
    if (best > threshold.ticks) {
        r.snapped = false;
        r.time = t;
    }
    return r;
}

RangeSnapResult snapMove(const std::vector<SnapPoint>& points, TimeRange original, Time delta, Time threshold) {
    RangeSnapResult r;
    r.delta = delta;
    const SnapResult a = snapTime(points, original.start + delta, threshold);
    const SnapResult b = snapTime(points, original.end() + delta, threshold);
    const int64_t da = a.snapped ? std::llabs((a.time - (original.start + delta)).ticks) : INT64_MAX;
    const int64_t db = b.snapped ? std::llabs((b.time - (original.end() + delta)).ticks) : INT64_MAX;
    if (da == INT64_MAX && db == INT64_MAX) return r;
    r.snapped = true;
    if (da <= db) {
        r.delta = a.time - original.start;
        r.point = a.point;
    } else {
        r.delta = b.time - original.end();
        r.point = b.point;
        r.snappedEnd = true;
    }
    return r;
}

}  // namespace avc
