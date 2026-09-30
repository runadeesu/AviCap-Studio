#pragma once
// Timeline edit operations. All functions operate through a SequenceEditor
// (copy-on-write) and keep the model invariants: per-track clips sorted and
// non-overlapping, positive durations, source ranges within media limits.
// They return Status::error(...) without partial changes being published when
// an operation is invalid (the Document discards the whole edit).

#include <optional>
#include <set>
#include <vector>

#include "core/result.h"
#include "timeline/editor.h"

namespace avc::edit {

enum class PlaceMode { Overwrite, Insert };
enum class Edge { Head, Tail };

// ---- primitives -------------------------------------------------------------

// Removes everything in `range` on one track (splitting/trimming clips that
// cross the boundaries). Leaves a gap.
Status clearRange(SequenceEditor& e, int trackIndex, TimeRange range);
// Shifts every clip starting at or after `from` by `delta` (may be negative;
// the caller guarantees room).
Status shiftClips(SequenceEditor& e, int trackIndex, Time from, Time delta);
// Opens a gap of `duration` at `at`, splitting a clip that spans `at`.
Status insertGap(SequenceEditor& e, int trackIndex, Time at, Time duration);
// True when no clip overlaps `range` on the track.
bool rangeIsEmpty(const Track& t, TimeRange range, const std::set<ClipId>& ignore = {});

// Internal trims without neighbour checks (source limits are enforced).
Status trimHeadTo(SequenceEditor& e, ClipId id, Time newStart);
Status trimTailTo(SequenceEditor& e, ClipId id, Time newEnd);

// ---- clip placement ------------------------------------------------------------

struct Placement {
    int trackIndex = -1;
    Clip clip;  // clip.start is the destination time
};

// Places clips (already carrying new ids). Overwrite clears their ranges;
// Insert ripples the target tracks (and sync-locked tracks) to make room.
Status placeClips(SequenceEditor& e, std::vector<Placement> placements, PlaceMode mode);

// Adds a media item: video part to `videoTrack` and linked audio part to
// `audioTrack` (either may be -1). Returns the created clip ids.
Result<std::vector<ClipId>> addMediaClip(SequenceEditor& e, const MediaItem& media, Time at, int videoTrack,
                                         int audioTrack, PlaceMode mode, std::optional<TimeRange> sourceRange = {});

// ---- basic edits ---------------------------------------------------------------

// Splits the clip at time t (and its linked partners spanning t).
// Returns ids of the new right-hand clips.
Result<std::vector<ClipId>> splitClip(SequenceEditor& e, ClipId id, Time t, bool includeLinked = true);
// Splits every clip crossing t on the given tracks (all unlocked tracks if empty).
Result<std::vector<ClipId>> splitAtTime(SequenceEditor& e, Time t, const std::vector<int>& tracks = {});

// Trims one edge. Non-ripple trims are limited by neighbours; ripple trims
// shift following clips on the clip's track and sync-locked tracks.
Status trimClip(SequenceEditor& e, ClipId id, Edge edge, Time newTime, bool ripple, bool includeLinked = true);
// Moves the edit point between two adjacent clips (roll edit).
Status rollEdit(SequenceEditor& e, ClipId left, ClipId right, Time newCut);
// Changes which part of the source is shown without moving the clip.
Status slipClip(SequenceEditor& e, ClipId id, Time sourceDelta, bool includeLinked = true);
// Moves a clip between its neighbours, trimming them to compensate.
Status slideClip(SequenceEditor& e, ClipId id, Time delta);

// Moves clips in time and across tracks. Track deltas apply within the clip's
// family (visual / audio).
Status moveClips(SequenceEditor& e, const std::set<ClipId>& ids, Time delta, int visualTrackDelta, int audioTrackDelta,
                 PlaceMode mode = PlaceMode::Overwrite);

Status deleteClips(SequenceEditor& e, const std::set<ClipId>& ids);  // leaves gaps
Status rippleDelete(SequenceEditor& e, const std::set<ClipId>& ids);
// Removes content in `range` on tracks (leave gap).
Status lift(SequenceEditor& e, TimeRange range, const std::vector<int>& tracks);
// Removes content in `range` on tracks and closes the gap.
Status extract(SequenceEditor& e, TimeRange range, const std::vector<int>& tracks);
// Closes the gap at time t on a track (ripple).
Status closeGap(SequenceEditor& e, int trackIndex, Time t);

Result<std::vector<ClipId>> duplicateClips(SequenceEditor& e, const std::set<ClipId>& ids);
Status replaceClipMedia(SequenceEditor& e, ClipId id, const MediaItem& media, Time sourceIn);

Status setClipsEnabled(SequenceEditor& e, const std::set<ClipId>& ids, bool enabled);
Status groupClips(SequenceEditor& e, const std::set<ClipId>& ids);
Status ungroupClips(SequenceEditor& e, const std::set<ClipId>& ids);
Status linkClips(SequenceEditor& e, const std::set<ClipId>& ids);
Status unlinkClips(SequenceEditor& e, const std::set<ClipId>& ids);

// Changes playback speed; timeline duration follows. With `ripple`, later
// clips move; otherwise the clip is limited by the next clip.
Status setClipSpeed(SequenceEditor& e, ClipId id, Rational speed, bool ripple);
Status setClipReverse(SequenceEditor& e, ClipId id, bool reverse);
// Splits at t and inserts a freeze frame of `duration` (ripple insert).
Result<ClipId> insertFreezeFrame(SequenceEditor& e, ClipId id, Time t, Time duration);

// ---- clipboard -------------------------------------------------------------------

struct ClipboardItem {
    int trackOffset = 0;  // relative to the lowest track in its family
    bool audioFamily = false;
    Clip clip;            // clip.start relative to clipboard origin
};
struct Clipboard {
    std::vector<ClipboardItem> items;
    [[nodiscard]] bool empty() const { return items.empty(); }
};
Clipboard copyClips(const Sequence& seq, const std::set<ClipId>& ids);
Result<std::vector<ClipId>> pasteClips(SequenceEditor& e, const Clipboard& cb, Time at, int visualBaseTrack,
                                       int audioBaseTrack, PlaceMode mode);

// ---- tracks ------------------------------------------------------------------------

// Inserts a track; visual tracks go above the topmost visual track by default
// (or at `familyPosition` among visual tracks), audio tracks below.
Result<TrackId> addTrack(SequenceEditor& e, TrackKind kind, std::string name = {}, int familyPosition = -1);
Status removeTrack(SequenceEditor& e, TrackId id);
Status moveTrack(SequenceEditor& e, TrackId id, int newFamilyPosition);

// ---- markers -------------------------------------------------------------------------

MarkerId addMarker(SequenceEditor& e, Marker m);
bool removeMarker(SequenceEditor& e, MarkerId id);
bool updateMarker(SequenceEditor& e, const Marker& m);
void removeMarkersOfKind(SequenceEditor& e, MarkerKind kind);

// ---- compound ---------------------------------------------------------------------------

// Moves the selected clips into a new sequence and replaces them with compound
// clips. Returns the new sequence id.
Result<SequenceId> createCompoundClip(ProjectEditor& pe, SequenceId seqId, const std::set<ClipId>& ids,
                                      std::string name);

// ---- helpers ------------------------------------------------------------------------------

// Expands a selection with linked partners and group members.
std::set<ClipId> expandSelection(const Sequence& seq, const std::set<ClipId>& ids, bool linked = true,
                                 bool groups = true);
// Tracks affected by ripple edits originating on `tracks`.
std::vector<int> rippleTracks(const Sequence& seq, const std::vector<int>& tracks);

}  // namespace avc::edit
