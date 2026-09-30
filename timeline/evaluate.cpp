#include "timeline/evaluate.h"

#include <algorithm>
#include <cmath>

namespace avc {

Time sourceTimeUnclamped(const Clip& c, Time t) {
    if (c.freezeFrame) return c.sourceIn;
    const Time local = t - c.start;
    Time src;
    if (c.reverse) src = c.sourceIn + (c.duration - local).scaled(c.speed, Rounding::Nearest) - Time{1};
    else src = c.sourceIn + local.scaled(c.speed, Rounding::Down);
    return src.ticks < 0 ? Time{0} : src;
}

std::vector<TransitionRegion> transitionRegions(const Track& track) {
    std::vector<TransitionRegion> out;
    const auto& clips = track.clips;
    for (size_t i = 0; i < clips.size(); ++i) {
        const Clip& c = *clips[i];
        if (!c.enabled) continue;
        const Clip* prev = (i > 0 && clips[i - 1]->end() == c.start && clips[i - 1]->enabled) ? clips[i - 1].get() : nullptr;
        const Clip* next = (i + 1 < clips.size() && clips[i + 1]->start == c.end() && clips[i + 1]->enabled) ? clips[i + 1].get() : nullptr;
        if (c.transitionIn && c.transitionIn->duration.ticks > 0) {
            Time d = c.transitionIn->duration;
            if (prev) {
                // Centered on the cut; each side contributes at most its own length.
                d = minTime(d, minTime(prev->duration, c.duration) * 2);
                const Time half{d.ticks / 2};
                out.push_back({TimeRange{c.start - half, d}, &*c.transitionIn, prev, &c});
            } else {
                d = minTime(d, c.duration);
                out.push_back({TimeRange{c.start, d}, &*c.transitionIn, nullptr, &c});
            }
        }
        if (c.transitionOut && c.transitionOut->duration.ticks > 0) {
            // A cross transition at this cut is owned by the next clip's transitionIn.
            if (next && next->transitionIn && next->transitionIn->duration.ticks > 0) continue;
            Time d = c.transitionOut->duration;
            if (next) {
                d = minTime(d, minTime(next->duration, c.duration) * 2);
                const Time half{d.ticks / 2};
                out.push_back({TimeRange{c.end() - half, d}, &*c.transitionOut, &c, next});
            } else {
                d = minTime(d, c.duration);
                out.push_back({TimeRange{c.end() - d, d}, &*c.transitionOut, &c, nullptr});
            }
        }
    }
    return out;
}

FramePlan planFrame(const ProjectPtr& project, const Sequence& seq, Time t) {
    FramePlan plan;
    plan.project = project;
    plan.sequence = &seq;
    plan.time = t;
    for (int ti : seq.visualTrackIndices()) {
        const Track& track = *seq.tracks[static_cast<size_t>(ti)];
        if (track.hidden) continue;
        // Transition regions take precedence over plain clips.
        bool handled = false;
        for (const auto& reg : transitionRegions(track)) {
            if (!reg.range.contains(t)) continue;
            VisualLayer layer;
            layer.trackIndex = ti;
            layer.transition = reg.spec;
            const double len = static_cast<double>(reg.range.duration.ticks);
            layer.progress = len > 0 ? static_cast<float>(static_cast<double>((t - reg.range.start).ticks) / len) : 1.0f;
            layer.progress = std::clamp(layer.progress, 0.0f, 1.0f);
            if (reg.outgoing) layer.a = {reg.outgoing, sourceTimeUnclamped(*reg.outgoing, t), t - reg.outgoing->start};
            if (reg.incoming) layer.b = {reg.incoming, sourceTimeUnclamped(*reg.incoming, t), t - reg.incoming->start};
            plan.layers.push_back(layer);
            handled = true;
            break;
        }
        if (handled) continue;
        const int idx = track.clipIndexAt(t);
        if (idx < 0) continue;
        const Clip& c = *track.clips[static_cast<size_t>(idx)];
        if (!c.enabled) continue;
        VisualLayer layer;
        layer.trackIndex = ti;
        layer.a = {&c, c.sourceTimeAt(t), t - c.start};
        plan.layers.push_back(layer);
    }
    return plan;
}

bool trackAudible(const Sequence& seq, int trackIndex) {
    const Track& t = *seq.tracks[static_cast<size_t>(trackIndex)];
    if (t.kind != TrackKind::Audio || t.muted) return false;
    bool anySolo = false;
    for (auto& tr : seq.tracks)
        if (tr->kind == TrackKind::Audio && tr->solo) anySolo = true;
    return !anySolo || t.solo;
}

std::vector<AudioClipRef> activeAudioClips(const Sequence& seq, TimeRange range) {
    std::vector<AudioClipRef> out;
    for (int ti : seq.audioTrackIndices()) {
        if (!trackAudible(seq, ti)) continue;
        const Track& t = *seq.tracks[static_cast<size_t>(ti)];
        // Include clips whose transition handles extend into the range.
        const Time margin = Time::fromSeconds(10.0);
        for (size_t i = t.lowerBound(range.start - margin); i < t.clips.size(); ++i) {
            const Clip& c = *t.clips[i];
            if (c.start >= range.end() + margin) break;
            if (!c.enabled) continue;
            TimeRange extended = c.range();
            if (c.transitionIn) {
                const Time h{c.transitionIn->duration.ticks / 2};
                extended = TimeRange::fromStartEnd(extended.start - h, extended.end());
            }
            if (c.transitionOut) {
                const Time h{c.transitionOut->duration.ticks / 2};
                extended = TimeRange::fromStartEnd(extended.start, extended.end() + h);
            }
            if (extended.overlaps(range)) out.push_back({ti, &c});
        }
    }
    return out;
}

float audioTransitionGain(const Track& track, const Clip& clip, Time t) {
    float gain = 1.0f;
    bool inRegion = false;
    for (const auto& reg : transitionRegions(track)) {
        if (!reg.range.contains(t)) continue;
        if (reg.outgoing != &clip && reg.incoming != &clip) continue;
        inRegion = true;
        const double len = static_cast<double>(reg.range.duration.ticks);
        const float p = len > 0 ? static_cast<float>(static_cast<double>((t - reg.range.start).ticks) / len) : 1.0f;
        // Equal-power crossfade.
        const float g = reg.incoming == &clip ? std::sin(p * 1.5707963f) : std::cos(p * 1.5707963f);
        gain *= g;
    }
    if (!inRegion && !clip.range().contains(t)) return 0.0f;
    return gain;
}

}  // namespace avc
