#pragma once
// Timeline evaluation: which clips are visible at a time, with which source
// times and transition mixes. Pure model logic shared by preview and export.

#include <optional>
#include <vector>

#include "timeline/model.h"

namespace avc {

struct ClipSample {
    const Clip* clip = nullptr;
    Time sourceTime;   // media time (or nested-sequence time) to fetch
    Time localTime;    // clip-local time for keyframe evaluation
    [[nodiscard]] explicit operator bool() const { return clip != nullptr; }
};

struct VisualLayer {
    int trackIndex = -1;
    ClipSample a;  // the clip, or the outgoing side of a transition (may be empty)
    ClipSample b;  // incoming side of a transition (may be empty)
    const TransitionSpec* transition = nullptr;
    float progress = 0.0f;  // 0 = fully A, 1 = fully B
    [[nodiscard]] bool isTransition() const { return transition != nullptr; }
};

struct FramePlan {
    ProjectPtr project;  // keeps every pointer below alive
    const Sequence* sequence = nullptr;
    Time time;
    std::vector<VisualLayer> layers;  // bottom -> top
};

// Source time for a timeline time, allowing evaluation outside the clip range
// (transition handles). Result is clamped at >= 0.
Time sourceTimeUnclamped(const Clip& c, Time t);

// Effective transition length/region at a clip edge (clamped to clip lengths).
struct TransitionRegion {
    TimeRange range;
    const TransitionSpec* spec = nullptr;
    const Clip* outgoing = nullptr;  // may be null (fade from empty)
    const Clip* incoming = nullptr;  // may be null (fade to empty)
};
std::vector<TransitionRegion> transitionRegions(const Track& track);

FramePlan planFrame(const ProjectPtr& project, const Sequence& seq, Time t);

// Audio clips overlapping `range` on unmuted tracks (respecting solo).
struct AudioClipRef {
    int trackIndex = -1;
    const Clip* clip = nullptr;
};
std::vector<AudioClipRef> activeAudioClips(const Sequence& seq, TimeRange range);
bool trackAudible(const Sequence& seq, int trackIndex);

// Fade gain (0..1) applied to an audio clip at timeline time t from its
// transitions (audio transitions are equal-power crossfades).
float audioTransitionGain(const Track& track, const Clip& clip, Time t);

}  // namespace avc
