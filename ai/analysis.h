#pragma once
// Media analysis used by the AI tools: silence, beats (tempo), scene changes
// and loudness highlights. Everything runs locally on worker threads; the
// sample-based functions are pure and unit tested.

#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/result.h"
#include "timeline/editor.h"
#include "timeline/model.h"

namespace avc::ai {

// ------------------------------------------------------------------ silence

struct SilenceOptions {
    bool autoThreshold = true;   // derive the threshold from the noise floor
    float thresholdDb = -40.0f;  // used when autoThreshold is false (dBFS RMS)
    double minSilenceSec = 0.5;  // shorter pauses are kept
    double paddingSec = 0.12;    // breathing room kept around speech on both sides
};

struct SilenceResult {
    std::vector<TimeRange> silences;  // media time, after padding
    float thresholdDb = 0;            // threshold actually used
    float noiseFloorDb = 0;
    double durationSec = 0;
};

// Mono samples -> silent ranges (media time = sample position).
SilenceResult detectSilenceInSamples(const float* mono, size_t frames, int sampleRate, const SilenceOptions& opt);
// Decodes (streaming, 16 kHz mono) and analyses a file.
Result<SilenceResult> detectSilence(const std::string& utf8Path, const SilenceOptions& opt, const CancelToken& cancel = {},
                                    JobContext* progress = nullptr);

// ------------------------------------------------------------------ beats

struct BeatResult {
    double bpm = 0;
    std::vector<Time> beats;  // media time
    float confidence = 0;     // 0..1 (periodicity strength)
};
BeatResult detectBeatsInSamples(const float* mono, size_t frames, int sampleRate);
Result<BeatResult> detectBeats(const std::string& utf8Path, const CancelToken& cancel = {}, JobContext* progress = nullptr);

// ------------------------------------------------------------------ scenes

struct SceneOptions {
    float threshold = 0.30f;     // histogram distance (0..1) that counts as a cut
    double minSceneSec = 0.5;    // ignore cuts closer than this
    int analysisWidth = 160;     // frames are downscaled for analysis
};
struct SceneResult {
    std::vector<Time> cuts;      // media time of the first frame of each new scene
    std::vector<float> scores;   // distance at each cut
};
Result<SceneResult> detectScenes(const std::string& utf8Path, const SceneOptions& opt, const CancelToken& cancel = {},
                                 JobContext* progress = nullptr);

// ------------------------------------------------------------------ highlights

struct Highlight {
    TimeRange range;  // media time
    float score = 0;  // loudness above the median, dB
};
// Loudest moments (e.g. laughter, cheering, emphasis) of a mono signal.
std::vector<Highlight> findHighlightsInSamples(const float* mono, size_t frames, int sampleRate, int maxCount, double windowSec);
Result<std::vector<Highlight>> findHighlights(const std::string& utf8Path, int maxCount, double windowSec, const CancelToken& cancel = {},
                                              JobContext* progress = nullptr);

// ------------------------------------------------------------------ timeline helpers

// Maps source-time ranges of a clip's media to timeline ranges (clipped to the clip).
std::vector<TimeRange> sourceRangesToTimeline(const Clip& clip, const std::vector<TimeRange>& sourceRanges);
// Maps a source time to the timeline (nullopt when outside the clip).
std::optional<Time> sourceTimeToTimeline(const Clip& clip, Time source);

// Sorts and merges ranges, dropping ones shorter than minLength.
std::vector<TimeRange> normalizeRanges(std::vector<TimeRange> ranges, Time minLength = Time{1});
// Removes the ranges from every track and closes the gaps (ripple), keeping
// all tracks in sync. Applied back to front in one edit.
Status rippleRemoveRanges(SequenceEditor& e, const std::vector<TimeRange>& ranges);

}  // namespace avc::ai
