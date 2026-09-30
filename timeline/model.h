#pragma once
// Timeline data model.
//
//   Project -> Sequence -> Track -> Clip -> (Transform/Audio/Text components,
//   Effects) -> Parameters -> Keyframes
//
// The model is immutable once published: containers hold shared_ptr<const T>
// and edits create new versions with structural sharing (see ProjectEditor).
// Render, audio and export threads therefore read a consistent snapshot
// without locks, and undo/redo simply swaps snapshot roots.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ids.h"
#include "core/time.h"
#include "timeline/params.h"

namespace avc {

using MediaId = Id;
using ClipId = Id;
using TrackId = Id;
using SequenceId = Id;
using BinId = Id;
using MarkerId = Id;

// ---------------------------------------------------------------- media

enum class MediaKind : uint8_t { Video, Audio, Image, Unknown };

struct VideoStreamInfo {
    int index = -1;  // container stream index
    int width = 0, height = 0;
    Rational frameRate{0, 1};
    Rational sampleAspect{1, 1};
    std::string codec;
    std::string pixelFormat;
    int bitDepth = 8;
    int rotation = 0;  // display matrix rotation in degrees (0/90/180/270)
    std::string colorSpace;     // bt709, bt2020nc, ...
    std::string colorTransfer;  // bt709, smpte2084 (PQ), arib-std-b67 (HLG)
    std::string colorPrimaries;
    bool fullRange = false;
    bool hasAlpha = false;
    int64_t frameCount = 0;
    bool operator==(const VideoStreamInfo&) const = default;
};

struct AudioStreamInfo {
    int index = -1;
    int sampleRate = 0;
    int channels = 0;
    std::string codec;
    std::string channelLayout;
    std::string language;
    bool operator==(const AudioStreamInfo&) const = default;
};

struct MediaInfo {
    MediaKind kind = MediaKind::Unknown;
    std::string container;
    Time duration;
    Time startTime;  // container start offset (normalized away by decoders)
    int64_t bitRate = 0;
    std::vector<VideoStreamInfo> video;
    std::vector<AudioStreamInfo> audio;
    std::string timecode;  // embedded start timecode, if any
    [[nodiscard]] bool hasVideo() const { return !video.empty(); }
    [[nodiscard]] bool hasAudio() const { return !audio.empty(); }
    [[nodiscard]] const VideoStreamInfo* primaryVideo() const { return video.empty() ? nullptr : &video[0]; }
    [[nodiscard]] const AudioStreamInfo* primaryAudio() const { return audio.empty() ? nullptr : &audio[0]; }
    bool operator==(const MediaInfo&) const = default;
};

struct MediaItem {
    MediaId id = kInvalidId;
    std::string name;
    std::string path;          // absolute, UTF-8
    std::string relativePath;  // relative to the project file (for relinking)
    uint64_t fileSize = 0;     // identity at import time
    int64_t fileModifiedNs = 0;
    MediaInfo info;
    BinId bin = kInvalidId;
    bool favorite = false;
    std::vector<std::string> tags;
    std::string notes;
    bool operator==(const MediaItem&) const = default;
};
using MediaItemPtr = std::shared_ptr<const MediaItem>;

struct Bin {
    BinId id = kInvalidId;
    BinId parent = kInvalidId;
    std::string name;
    bool operator==(const Bin&) const = default;
};

// ---------------------------------------------------------------- clips

enum class ClipKind : uint8_t {
    Media,       // video / audio / image from a MediaItem
    Text,        // title
    Subtitle,    // caption (text on a subtitle track)
    Solid,       // colour matte
    Adjustment,  // applies its effects to everything below
    Compound,    // nested sequence (compound clip)
};

enum class BlendMode : uint8_t {
    Normal = 0, Multiply, Screen, Overlay, SoftLight, HardLight, Add, Difference, Darken, Lighten, Count
};

const char* clipKindName(ClipKind k);
ClipKind clipKindFromName(std::string_view s);
const char* blendModeName(BlendMode m);
BlendMode blendModeFromName(std::string_view s);

struct EffectInstance {
    Id id = kInvalidId;
    std::string effectId;  // registry id, e.g. "blur.gaussian"
    bool enabled = true;
    ParamSet params;
    bool operator==(const EffectInstance&) const = default;
};

struct TransitionSpec {
    std::string type = "cross-dissolve";  // registry id
    Time duration;
    ParamSet params;
    bool operator==(const TransitionSpec&) const = default;
};

enum class TextAlign : uint8_t { Left, Center, Right };

struct TextStyle {
    std::string fontFamily = "Yu Gothic UI";
    float fontSize = 72.0f;  // pixels at sequence resolution
    int fontWeight = 700;    // 100..900
    bool italic = false;
    TextAlign align = TextAlign::Center;
    float tracking = 0.0f;     // extra letter spacing, 1/1000 em
    float lineSpacing = 1.2f;  // multiple of font size
    float kerningEnabled = 1.0f;
    // Colours/strengths are keyframable and live in Clip::textParams:
    //   fillColor, strokeColor, strokeWidth, shadowColor, shadowOffset, shadowBlur,
    //   backgroundColor, backgroundPadding, glowColor, glowRadius, gradientColor,
    //   gradientEnabled, reveal (0..1 typewriter)
    bool operator==(const TextStyle&) const = default;
};

struct Clip {
    ClipId id = kInvalidId;
    ClipKind kind = ClipKind::Media;
    std::string name;

    MediaId media = kInvalidId;         // ClipKind::Media
    SequenceId nested = kInvalidId;     // ClipKind::Compound
    int streamIndex = -1;               // container stream used (-1: primary of the track type)

    Time start;      // position on the timeline
    Time duration;   // length on the timeline
    Time sourceIn;   // first source time used (media time for Media, nested time for Compound)

    Rational speed{1, 1};  // playback rate (> 0)
    bool reverse = false;
    bool freezeFrame = false;  // holds the frame at sourceIn

    bool enabled = true;
    Id linkGroup = kInvalidId;  // linked A/V partners share this id
    Id group = kInvalidId;      // user group
    uint32_t colorLabel = 0;    // 0 = default for kind

    ParamSet transform;  // position, scale, rotation, anchor, opacity, crop*
    bool flipH = false, flipV = false;
    BlendMode blend = BlendMode::Normal;
    ParamSet audio;      // volume (dB), pan (-1..1)
    float gainDb = 0.0f;
    std::vector<EffectInstance> effects;  // video (or audio for audio clips), in order

    std::optional<TransitionSpec> transitionIn;
    std::optional<TransitionSpec> transitionOut;

    // Text / subtitle / solid
    std::string text;
    TextStyle textStyle;
    ParamSet textParams;
    ParamValue solidColor{0.f, 0.f, 0.f, 1.f};

    [[nodiscard]] Time end() const noexcept { return start + duration; }
    [[nodiscard]] TimeRange range() const noexcept { return {start, duration}; }
    // Amount of source consumed by the clip.
    [[nodiscard]] Time sourceDuration() const noexcept {
        return freezeFrame ? Time{0} : duration.scaled(speed, Rounding::Nearest);
    }
    [[nodiscard]] Time sourceOut() const noexcept { return sourceIn + sourceDuration(); }
    // Maps a timeline time to source time (clamped into the clip).
    [[nodiscard]] Time sourceTimeAt(Time t) const noexcept;
    // Keyframe-local time for a timeline time.
    [[nodiscard]] Time localTime(Time t) const noexcept { return t - start; }
    [[nodiscard]] bool hasLimitedSource() const noexcept { return kind == ClipKind::Media || kind == ClipKind::Compound; }

    bool operator==(const Clip&) const = default;
};
using ClipPtr = std::shared_ptr<const Clip>;

// ---------------------------------------------------------------- tracks

enum class TrackKind : uint8_t { Video, Audio, Text, Subtitle, Adjustment, Effect };

const char* trackKindName(TrackKind k);
TrackKind trackKindFromName(std::string_view s);
inline bool isVisualTrack(TrackKind k) { return k != TrackKind::Audio; }
// Whether a clip kind may live on a track kind.
bool trackAccepts(TrackKind track, ClipKind clip, bool mediaHasVideo, bool mediaHasAudio);

struct Track {
    TrackId id = kInvalidId;
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool locked = false;
    bool hidden = false;  // visual tracks: not rendered
    bool muted = false;   // audio tracks
    bool solo = false;
    bool syncLock = true;  // participates in ripple edits of other tracks
    float height = 0.0f;   // UI height (0 = default)
    float volumeDb = 0.0f;
    float pan = 0.0f;
    std::vector<EffectInstance> effects;  // audio track inserts
    std::vector<ClipPtr> clips;           // sorted by start, non-overlapping

    [[nodiscard]] int indexOf(ClipId id) const noexcept;
    [[nodiscard]] const Clip* find(ClipId id) const noexcept;
    // Index of the clip containing t, or -1.
    [[nodiscard]] int clipIndexAt(Time t) const noexcept;
    // First clip index whose end > t.
    [[nodiscard]] size_t lowerBound(Time t) const noexcept;
    [[nodiscard]] Time endTime() const noexcept { return clips.empty() ? Time{0} : clips.back()->end(); }

    bool operator==(const Track&) const = default;
};
using TrackPtr = std::shared_ptr<const Track>;

// ---------------------------------------------------------------- sequences

enum class MarkerKind : uint8_t { Standard, Chapter, Beat, Scene, Silence, Highlight, Todo };
const char* markerKindName(MarkerKind k);
MarkerKind markerKindFromName(std::string_view s);

struct Marker {
    MarkerId id = kInvalidId;
    Time time;
    Time duration;
    std::string name;
    std::string comment;
    uint32_t color = 0xFF3CB4E6;  // ABGR
    MarkerKind kind = MarkerKind::Standard;
    float score = 0.0f;  // analysis confidence (highlights, beats)
    bool operator==(const Marker&) const = default;
};

struct Sequence {
    SequenceId id = kInvalidId;
    std::string name;
    int width = 1920;
    int height = 1080;
    Rational frameRate{30, 1};
    int sampleRate = 48000;
    int audioChannels = 2;
    std::vector<TrackPtr> tracks;  // visual tracks bottom->top, then audio tracks
    std::vector<Marker> markers;   // sorted by time
    float masterVolumeDb = 0.0f;
    std::vector<EffectInstance> masterEffects;
    std::optional<TimeRange> workArea;  // in/out for export & playback loop
    ParamValue backgroundColor{0.f, 0.f, 0.f, 1.f};

    [[nodiscard]] Time duration() const noexcept;
    [[nodiscard]] Time frameDuration() const noexcept { return Time::frameDuration(frameRate); }
    [[nodiscard]] int trackIndex(TrackId id) const noexcept;
    [[nodiscard]] const Track* findTrack(TrackId id) const noexcept;
    struct ClipRef {
        int track = -1;
        int clip = -1;
        explicit operator bool() const { return track >= 0; }
    };
    [[nodiscard]] ClipRef findClip(ClipId id) const noexcept;
    [[nodiscard]] const Clip* clip(ClipId id) const noexcept;
    [[nodiscard]] const Track* trackOfClip(ClipId id) const noexcept;
    // Clips sharing the link group of `id` (including itself).
    [[nodiscard]] std::vector<ClipId> linkedClips(ClipId id) const;
    [[nodiscard]] std::vector<ClipId> groupedClips(ClipId id) const;
    [[nodiscard]] int countTracks(TrackKind k) const noexcept;
    // Tracks of one family in display order (visual: bottom->top).
    [[nodiscard]] std::vector<int> visualTrackIndices() const;
    [[nodiscard]] std::vector<int> audioTrackIndices() const;
    [[nodiscard]] size_t clipCount() const noexcept;

    bool operator==(const Sequence&) const = default;
};
using SequencePtr = std::shared_ptr<const Sequence>;

// ---------------------------------------------------------------- project

inline constexpr int kProjectSchemaVersion = 1;

struct ProjectSettings {
    int width = 1920;
    int height = 1080;
    Rational frameRate{30, 1};
    int sampleRate = 48000;
    bool operator==(const ProjectSettings&) const = default;
};

struct Project {
    Id id = kInvalidId;
    std::string name = "Untitled";
    ProjectSettings settings;
    std::vector<MediaItemPtr> media;
    std::vector<Bin> bins;
    std::vector<SequencePtr> sequences;
    SequenceId activeSequence = kInvalidId;
    std::string createdUtc;
    std::string appVersion;

    [[nodiscard]] const MediaItem* findMedia(MediaId id) const noexcept;
    [[nodiscard]] int mediaIndex(MediaId id) const noexcept;
    [[nodiscard]] const Sequence* findSequence(SequenceId id) const noexcept;
    [[nodiscard]] int sequenceIndex(SequenceId id) const noexcept;
    [[nodiscard]] const Sequence* active() const noexcept { return findSequence(activeSequence); }

    bool operator==(const Project&) const = default;
};
using ProjectPtr = std::shared_ptr<const Project>;

// Factory helpers ------------------------------------------------------------

// Default transform parameter set (all keyframable parameters present).
ParamSet defaultTransformParams();
ParamSet defaultAudioParams();
ParamSet defaultTextParams();
const std::vector<ParamDef>& transformParamDefs();
const std::vector<ParamDef>& audioParamDefs();
const std::vector<ParamDef>& textParamDefs();

Clip makeMediaClip(const MediaItem& media, TrackKind trackKind, Time start, Time sourceIn, Time duration);
Clip makeTextClip(std::string text, Time start, Time duration, ClipKind kind = ClipKind::Text);
Clip makeSolidClip(ParamValue color, Time start, Time duration);
Clip makeAdjustmentClip(Time start, Time duration);
Track makeTrack(TrackKind kind, std::string name);
Sequence makeSequence(std::string name, int width, int height, Rational fps, int videoTracks = 3, int audioTracks = 3);
Project makeProject(std::string name, const ProjectSettings& settings = {});

// Default image/still duration when placed on the timeline.
inline Time defaultStillDuration() { return Time::fromSeconds(5.0); }

// Checks all structural invariants (sorted, non-overlapping clips, unique ids,
// positive durations, valid references). Returns an error description or "".
std::string validateSequence(const Sequence& seq);
std::string validateProject(const Project& project);

}  // namespace avc
