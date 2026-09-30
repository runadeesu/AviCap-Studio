#include "timeline/model.h"

#include <algorithm>
#include <set>

namespace avc {

// ---------------------------------------------------------------- names

const char* clipKindName(ClipKind k) {
    switch (k) {
    case ClipKind::Media: return "media";
    case ClipKind::Text: return "text";
    case ClipKind::Subtitle: return "subtitle";
    case ClipKind::Solid: return "solid";
    case ClipKind::Adjustment: return "adjustment";
    case ClipKind::Compound: return "compound";
    }
    return "media";
}

ClipKind clipKindFromName(std::string_view s) {
    if (s == "text") return ClipKind::Text;
    if (s == "subtitle") return ClipKind::Subtitle;
    if (s == "solid") return ClipKind::Solid;
    if (s == "adjustment") return ClipKind::Adjustment;
    if (s == "compound") return ClipKind::Compound;
    return ClipKind::Media;
}

const char* blendModeName(BlendMode m) {
    switch (m) {
    case BlendMode::Normal: return "normal";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen: return "screen";
    case BlendMode::Overlay: return "overlay";
    case BlendMode::SoftLight: return "softLight";
    case BlendMode::HardLight: return "hardLight";
    case BlendMode::Add: return "add";
    case BlendMode::Difference: return "difference";
    case BlendMode::Darken: return "darken";
    case BlendMode::Lighten: return "lighten";
    case BlendMode::Count: break;
    }
    return "normal";
}

BlendMode blendModeFromName(std::string_view s) {
    for (int i = 0; i < static_cast<int>(BlendMode::Count); ++i)
        if (s == blendModeName(static_cast<BlendMode>(i))) return static_cast<BlendMode>(i);
    return BlendMode::Normal;
}

const char* trackKindName(TrackKind k) {
    switch (k) {
    case TrackKind::Video: return "video";
    case TrackKind::Audio: return "audio";
    case TrackKind::Text: return "text";
    case TrackKind::Subtitle: return "subtitle";
    case TrackKind::Adjustment: return "adjustment";
    case TrackKind::Effect: return "effect";
    }
    return "video";
}

TrackKind trackKindFromName(std::string_view s) {
    if (s == "audio") return TrackKind::Audio;
    if (s == "text") return TrackKind::Text;
    if (s == "subtitle") return TrackKind::Subtitle;
    if (s == "adjustment") return TrackKind::Adjustment;
    if (s == "effect") return TrackKind::Effect;
    return TrackKind::Video;
}

const char* markerKindName(MarkerKind k) {
    switch (k) {
    case MarkerKind::Standard: return "standard";
    case MarkerKind::Chapter: return "chapter";
    case MarkerKind::Beat: return "beat";
    case MarkerKind::Scene: return "scene";
    case MarkerKind::Silence: return "silence";
    case MarkerKind::Highlight: return "highlight";
    case MarkerKind::Todo: return "todo";
    }
    return "standard";
}

MarkerKind markerKindFromName(std::string_view s) {
    if (s == "chapter") return MarkerKind::Chapter;
    if (s == "beat") return MarkerKind::Beat;
    if (s == "scene") return MarkerKind::Scene;
    if (s == "silence") return MarkerKind::Silence;
    if (s == "highlight") return MarkerKind::Highlight;
    if (s == "todo") return MarkerKind::Todo;
    return MarkerKind::Standard;
}

bool trackAccepts(TrackKind track, ClipKind clip, bool mediaHasVideo, bool mediaHasAudio) {
    switch (track) {
    case TrackKind::Video:
        if (clip == ClipKind::Media) return mediaHasVideo;
        return clip != ClipKind::Subtitle;
    case TrackKind::Audio:
        if (clip == ClipKind::Media) return mediaHasAudio;
        return clip == ClipKind::Compound;
    case TrackKind::Text: return clip == ClipKind::Text;
    case TrackKind::Subtitle: return clip == ClipKind::Subtitle;
    case TrackKind::Adjustment:
    case TrackKind::Effect: return clip == ClipKind::Adjustment;
    }
    return false;
}

// ---------------------------------------------------------------- Clip

Time Clip::sourceTimeAt(Time t) const noexcept {
    Time local = t - start;
    if (local.ticks < 0) local = Time{0};
    if (duration.ticks > 0 && local >= duration) local = duration - Time{1};
    if (freezeFrame) return sourceIn;
    if (reverse) {
        const Time remaining = duration - local;
        const Time src = sourceIn + remaining.scaled(speed, Rounding::Nearest) - Time{1};
        return src < sourceIn ? sourceIn : src;
    }
    return sourceIn + local.scaled(speed, Rounding::Down);
}

// ---------------------------------------------------------------- Track

int Track::indexOf(ClipId id) const noexcept {
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i]->id == id) return static_cast<int>(i);
    return -1;
}

const Clip* Track::find(ClipId id) const noexcept {
    const int i = indexOf(id);
    return i >= 0 ? clips[static_cast<size_t>(i)].get() : nullptr;
}

size_t Track::lowerBound(Time t) const noexcept {
    // First clip whose end is > t.
    auto it = std::partition_point(clips.begin(), clips.end(), [t](const ClipPtr& c) { return c->end() <= t; });
    return static_cast<size_t>(it - clips.begin());
}

int Track::clipIndexAt(Time t) const noexcept {
    const size_t i = lowerBound(t);
    if (i < clips.size() && clips[i]->start <= t) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------- Sequence

Time Sequence::duration() const noexcept {
    Time d{0};
    for (auto& t : tracks) d = maxTime(d, t->endTime());
    return d;
}

int Sequence::trackIndex(TrackId id) const noexcept {
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->id == id) return static_cast<int>(i);
    return -1;
}

const Track* Sequence::findTrack(TrackId id) const noexcept {
    const int i = trackIndex(id);
    return i >= 0 ? tracks[static_cast<size_t>(i)].get() : nullptr;
}

Sequence::ClipRef Sequence::findClip(ClipId id) const noexcept {
    for (size_t t = 0; t < tracks.size(); ++t) {
        const int c = tracks[t]->indexOf(id);
        if (c >= 0) return {static_cast<int>(t), c};
    }
    return {};
}

const Clip* Sequence::clip(ClipId id) const noexcept {
    auto r = findClip(id);
    return r ? tracks[static_cast<size_t>(r.track)]->clips[static_cast<size_t>(r.clip)].get() : nullptr;
}

const Track* Sequence::trackOfClip(ClipId id) const noexcept {
    auto r = findClip(id);
    return r ? tracks[static_cast<size_t>(r.track)].get() : nullptr;
}

std::vector<ClipId> Sequence::linkedClips(ClipId id) const {
    const Clip* c = clip(id);
    if (!c) return {};
    if (c->linkGroup == kInvalidId) return {id};
    std::vector<ClipId> out;
    for (auto& t : tracks)
        for (auto& cl : t->clips)
            if (cl->linkGroup == c->linkGroup) out.push_back(cl->id);
    return out;
}

std::vector<ClipId> Sequence::groupedClips(ClipId id) const {
    const Clip* c = clip(id);
    if (!c) return {};
    if (c->group == kInvalidId) return {id};
    std::vector<ClipId> out;
    for (auto& t : tracks)
        for (auto& cl : t->clips)
            if (cl->group == c->group) out.push_back(cl->id);
    return out;
}

int Sequence::countTracks(TrackKind k) const noexcept {
    int n = 0;
    for (auto& t : tracks)
        if (t->kind == k) ++n;
    return n;
}

std::vector<int> Sequence::visualTrackIndices() const {
    std::vector<int> out;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (isVisualTrack(tracks[i]->kind)) out.push_back(static_cast<int>(i));
    return out;
}

std::vector<int> Sequence::audioTrackIndices() const {
    std::vector<int> out;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (!isVisualTrack(tracks[i]->kind)) out.push_back(static_cast<int>(i));
    return out;
}

size_t Sequence::clipCount() const noexcept {
    size_t n = 0;
    for (auto& t : tracks) n += t->clips.size();
    return n;
}

// ---------------------------------------------------------------- Project

const MediaItem* Project::findMedia(MediaId id) const noexcept {
    const int i = mediaIndex(id);
    return i >= 0 ? media[static_cast<size_t>(i)].get() : nullptr;
}

int Project::mediaIndex(MediaId id) const noexcept {
    for (size_t i = 0; i < media.size(); ++i)
        if (media[i]->id == id) return static_cast<int>(i);
    return -1;
}

const Sequence* Project::findSequence(SequenceId id) const noexcept {
    const int i = sequenceIndex(id);
    return i >= 0 ? sequences[static_cast<size_t>(i)].get() : nullptr;
}

int Project::sequenceIndex(SequenceId id) const noexcept {
    for (size_t i = 0; i < sequences.size(); ++i)
        if (sequences[i]->id == id) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------- defaults

const std::vector<ParamDef>& transformParamDefs() {
    static const std::vector<ParamDef> defs = {
        {"position", "Position", ParamType::Vec2, pv(0, 0), -4000, 4000, 1.0f, true, {}, "px", "Transform"},
        {"scale", "Scale", ParamType::Vec2, pv(100, 100), 0, 800, 0.5f, true, {}, "%", "Transform"},
        {"rotation", "Rotation", ParamType::Angle, pv(0), -360, 360, 0.5f, true, {}, "°", "Transform"},
        {"anchor", "Anchor", ParamType::Vec2, pv(0, 0), -4000, 4000, 1.0f, true, {}, "px", "Transform"},
        {"opacity", "Opacity", ParamType::Percent, pv(100), 0, 100, 0.5f, true, {}, "%", "Opacity"},
        {"cropLeft", "Crop Left", ParamType::Percent, pv(0), 0, 100, 0.1f, true, {}, "%", "Crop"},
        {"cropRight", "Crop Right", ParamType::Percent, pv(0), 0, 100, 0.1f, true, {}, "%", "Crop"},
        {"cropTop", "Crop Top", ParamType::Percent, pv(0), 0, 100, 0.1f, true, {}, "%", "Crop"},
        {"cropBottom", "Crop Bottom", ParamType::Percent, pv(0), 0, 100, 0.1f, true, {}, "%", "Crop"},
        {"fitMode", "Fit", ParamType::Enum, pv(0), 0, 3, 1.0f, false, {"Fit", "Fill", "Stretch", "None"}, "", "Transform"},
    };
    return defs;
}

const std::vector<ParamDef>& audioParamDefs() {
    static const std::vector<ParamDef> defs = {
        {"volume", "Volume", ParamType::Decibel, pv(0), -60, 15, 0.1f, true, {}, "dB", "Audio"},
        {"pan", "Pan", ParamType::Float, pv(0), -1, 1, 0.01f, true, {}, "", "Audio"},
    };
    return defs;
}

const std::vector<ParamDef>& textParamDefs() {
    static const std::vector<ParamDef> defs = {
        {"fillColor", "Fill", ParamType::Color, pv(1, 1, 1, 1), 0, 1, 0.01f, true, {}, "", "Fill"},
        {"gradientEnabled", "Gradient", ParamType::Bool, pv(0), 0, 1, 1, false, {}, "", "Fill"},
        {"gradientColor", "Gradient Color", ParamType::Color, pv(1.0f, 0.75f, 0.2f, 1), 0, 1, 0.01f, true, {}, "", "Fill"},
        {"strokeColor", "Stroke Color", ParamType::Color, pv(0, 0, 0, 1), 0, 1, 0.01f, true, {}, "", "Stroke"},
        {"strokeWidth", "Stroke Width", ParamType::Float, pv(0), 0, 60, 0.5f, true, {}, "px", "Stroke"},
        {"shadowColor", "Shadow Color", ParamType::Color, pv(0, 0, 0, 0.6f), 0, 1, 0.01f, true, {}, "", "Shadow"},
        {"shadowOffset", "Shadow Offset", ParamType::Vec2, pv(4, 4), -100, 100, 0.5f, true, {}, "px", "Shadow"},
        {"shadowBlur", "Shadow Blur", ParamType::Float, pv(6), 0, 100, 0.5f, true, {}, "px", "Shadow"},
        {"backgroundColor", "Background", ParamType::Color, pv(0, 0, 0, 0), 0, 1, 0.01f, true, {}, "", "Background"},
        {"backgroundPadding", "Background Padding", ParamType::Float, pv(16), 0, 200, 1, true, {}, "px", "Background"},
        {"glowColor", "Glow Color", ParamType::Color, pv(1, 1, 1, 0), 0, 1, 0.01f, true, {}, "", "Glow"},
        {"glowRadius", "Glow Radius", ParamType::Float, pv(0), 0, 100, 0.5f, true, {}, "px", "Glow"},
        {"reveal", "Reveal", ParamType::Percent, pv(100), 0, 100, 0.5f, true, {}, "%", "Animation"},
        {"boxWidth", "Wrap Width", ParamType::Float, pv(0), 0, 8000, 1, false, {}, "px", "Layout"},
    };
    return defs;
}

namespace {
ParamSet fromDefs(const std::vector<ParamDef>& defs) {
    ParamSet s;
    for (auto& d : defs) s.setStatic(d.id, d.def);
    return s;
}
}  // namespace

ParamSet defaultTransformParams() { return fromDefs(transformParamDefs()); }
ParamSet defaultAudioParams() { return fromDefs(audioParamDefs()); }
ParamSet defaultTextParams() { return fromDefs(textParamDefs()); }

Clip makeMediaClip(const MediaItem& media, TrackKind trackKind, Time start, Time sourceIn, Time duration) {
    Clip c;
    c.id = newId();
    c.kind = ClipKind::Media;
    c.name = media.name;
    c.media = media.id;
    c.start = start;
    c.sourceIn = sourceIn;
    c.duration = duration;
    if (trackKind == TrackKind::Audio) {
        c.audio = defaultAudioParams();
        c.streamIndex = media.info.primaryAudio() ? media.info.primaryAudio()->index : -1;
    } else {
        c.transform = defaultTransformParams();
        c.streamIndex = media.info.primaryVideo() ? media.info.primaryVideo()->index : -1;
    }
    return c;
}

Clip makeTextClip(std::string text, Time start, Time duration, ClipKind kind) {
    Clip c;
    c.id = newId();
    c.kind = kind;
    c.name = text.size() > 32 ? text.substr(0, 32) : text;
    c.text = std::move(text);
    c.start = start;
    c.duration = duration;
    c.transform = defaultTransformParams();
    c.textParams = defaultTextParams();
    if (kind == ClipKind::Subtitle) {
        c.textStyle.fontSize = 54.0f;
        c.textStyle.fontWeight = 600;
        c.textParams.setStatic("strokeWidth", pv(4));
        c.textParams.setStatic("shadowColor", pv(0, 0, 0, 0.5f));
    }
    return c;
}

Clip makeSolidClip(ParamValue color, Time start, Time duration) {
    Clip c;
    c.id = newId();
    c.kind = ClipKind::Solid;
    c.name = "Color Matte";
    c.solidColor = color;
    c.start = start;
    c.duration = duration;
    c.transform = defaultTransformParams();
    return c;
}

Clip makeAdjustmentClip(Time start, Time duration) {
    Clip c;
    c.id = newId();
    c.kind = ClipKind::Adjustment;
    c.name = "Adjustment Layer";
    c.start = start;
    c.duration = duration;
    c.transform = defaultTransformParams();
    return c;
}

Track makeTrack(TrackKind kind, std::string name) {
    Track t;
    t.id = newId();
    t.kind = kind;
    t.name = std::move(name);
    return t;
}

Sequence makeSequence(std::string name, int width, int height, Rational fps, int videoTracks, int audioTracks) {
    Sequence s;
    s.id = newId();
    s.name = std::move(name);
    s.width = width;
    s.height = height;
    s.frameRate = fps.reduced();
    for (int i = 0; i < videoTracks; ++i)
        s.tracks.push_back(std::make_shared<Track>(makeTrack(TrackKind::Video, "V" + std::to_string(i + 1))));
    for (int i = 0; i < audioTracks; ++i)
        s.tracks.push_back(std::make_shared<Track>(makeTrack(TrackKind::Audio, "A" + std::to_string(i + 1))));
    return s;
}

Project makeProject(std::string name, const ProjectSettings& settings) {
    Project p;
    p.id = newId();
    p.name = std::move(name);
    p.settings = settings;
    auto seq = std::make_shared<Sequence>(
        makeSequence("Sequence 1", settings.width, settings.height, settings.frameRate, 3, 3));
    seq->sampleRate = settings.sampleRate;
    p.activeSequence = seq->id;
    p.sequences.push_back(seq);
    return p;
}

// ---------------------------------------------------------------- validation

std::string validateSequence(const Sequence& seq) {
    std::set<Id> trackIds, clipIds;
    if (seq.width <= 0 || seq.height <= 0) return "invalid sequence size";
    if (seq.frameRate.num <= 0 || seq.frameRate.den <= 0) return "invalid frame rate";
    bool seenAudio = false;
    for (auto& tp : seq.tracks) {
        if (!tp) return "null track";
        const Track& t = *tp;
        if (t.id == kInvalidId || !trackIds.insert(t.id).second) return "duplicate/invalid track id";
        if (t.kind == TrackKind::Audio) seenAudio = true;
        else if (seenAudio) return "visual track after audio tracks";
        Time prevEnd{std::numeric_limits<int64_t>::min()};
        for (auto& cp : t.clips) {
            if (!cp) return "null clip";
            const Clip& c = *cp;
            if (c.id == kInvalidId || !clipIds.insert(c.id).second) return "duplicate/invalid clip id";
            if (c.duration.ticks <= 0) return "non-positive clip duration in track " + t.name;
            if (c.start < prevEnd) return "overlapping or unsorted clips in track " + t.name;
            if (c.start.ticks < 0) return "negative clip start in track " + t.name;
            if (c.speed.num <= 0 || c.speed.den <= 0) return "invalid clip speed";
            if (c.sourceIn.ticks < 0) return "negative source in";
            prevEnd = c.end();
        }
    }
    for (size_t i = 1; i < seq.markers.size(); ++i)
        if (seq.markers[i].time < seq.markers[i - 1].time) return "markers not sorted";
    return {};
}

std::string validateProject(const Project& project) {
    std::set<Id> mediaIds;
    for (auto& m : project.media) {
        if (!m || m->id == kInvalidId || !mediaIds.insert(m->id).second) return "duplicate/invalid media id";
    }
    std::set<Id> seqIds;
    for (auto& s : project.sequences) {
        if (!s || !seqIds.insert(s->id).second) return "duplicate/invalid sequence id";
        std::string e = validateSequence(*s);
        if (!e.empty()) return s->name + ": " + e;
        for (auto& t : s->tracks)
            for (auto& c : t->clips) {
                if (c->kind == ClipKind::Media && !mediaIds.count(c->media)) return "clip references missing media";
                if (c->kind == ClipKind::Compound && !project.findSequence(c->nested))
                    return "compound clip references missing sequence";
            }
    }
    if (!project.sequences.empty() && !project.findSequence(project.activeSequence)) return "invalid active sequence";
    return {};
}

}  // namespace avc
