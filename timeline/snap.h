#pragma once
// Magnetic timeline snapping.

#include <set>
#include <vector>

#include "timeline/model.h"

namespace avc {

enum class SnapKind : uint8_t { ClipStart, ClipEnd, Playhead, Marker, Beat, Keyframe, Transition, WorkArea };

struct SnapPoint {
    Time time;
    SnapKind kind = SnapKind::ClipStart;
    Id source = kInvalidId;  // clip / marker id
    int track = -1;
};

struct SnapOptions {
    bool clips = true;
    bool playhead = true;
    bool markers = true;
    bool beats = true;
    bool keyframes = true;
    bool transitions = true;
};

// Collects snap targets sorted by time. Clips in `exclude` (the ones being
// dragged) are ignored. `keyframeClips` lists clips whose keyframes are shown.
std::vector<SnapPoint> collectSnapPoints(const Sequence& seq, Time playhead, const std::set<ClipId>& exclude,
                                         const SnapOptions& opt = {}, const std::set<ClipId>& keyframeClips = {});

struct SnapResult {
    bool snapped = false;
    Time time;        // snapped time (or input when not snapped)
    SnapPoint point;  // target that was hit
};

// Snaps a single time to the nearest point within `threshold`.
SnapResult snapTime(const std::vector<SnapPoint>& points, Time t, Time threshold);

struct RangeSnapResult {
    bool snapped = false;
    Time delta;          // adjusted move delta
    SnapPoint point;
    bool snappedEnd = false;  // true when the range end (not start) snapped
};

// Snaps a moving range (start and end edges) and returns the adjusted delta.
RangeSnapResult snapMove(const std::vector<SnapPoint>& points, TimeRange original, Time delta, Time threshold);

}  // namespace avc
