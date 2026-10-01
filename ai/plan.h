#pragma once
// Natural-language editing: Intent -> Plan -> Commands -> Preview -> Apply.
//
// A Plan is a list of validated operations (JSON-serializable, so the same
// format comes from the offline rule parser or from an opt-in cloud model).
// Plans are applied to a ProjectEditor in one go: the caller previews the
// result on a scratch copy and commits it as a single undoable edit, so a
// plan is always all-or-nothing in the undo history and the journal.

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ai/analysis.h"
#include "core/result.h"
#include "timeline/editor.h"

namespace avc::ai {

using Json = nlohmann::json;

// Supported operations (op id -> arguments):
//   remove_silence  {threshold_db?, min_silence?, padding?, track?}
//   delete_range    {start, end}                      seconds
//   keep_range      {start, end}                      deletes everything else
//   split_at        {time}
//   add_text        {text, start?, duration?}
//   add_subtitle    {text, start?, duration?}
//   set_volume      {target: "music"|"voice"|"all"|"selected"|<track name>, db}
//   apply_effect    {effect, target: "all_video"|"selected"|"all_audio", params?}
//   set_speed       {factor, target: "selected"|"all_video"}
//   fade            {direction: "in"|"out"|"both", duration?}
//   add_beat_markers {}
//   split_at_scenes {target: "selected"|"all_video"}
//   add_marker      {time, name?}
//   export          {preset}                          handled by the app, not an edit
struct PlanStep {
    std::string op;
    Json args = Json::object();
    std::string description;  // human readable (UI language)
};

struct Plan {
    std::vector<PlanStep> steps;
    std::string source;  // "local" or the cloud model id
    std::vector<std::string> notes;  // what was not understood / assumptions
    [[nodiscard]] bool empty() const { return steps.empty(); }
    [[nodiscard]] Json toJson() const;
};

const std::vector<std::string>& supportedOps();
// Validates a plan coming from outside (cloud model); unknown ops or bad
// arguments are rejected with a message instead of being guessed.
Result<Plan> planFromJson(const Json& j, const std::string& source);

// Offline rule-based understanding of Japanese and English instructions.
struct PlanContext {
    Time playhead;
    Time sequenceDuration;
    bool hasSelection = false;
};
Plan planFromText(const std::string& text, const PlanContext& ctx);
// Example instructions shown in the UI (English keys; tr() gives the
// Japanese text, and both are understood by planFromText).
const std::vector<const char*>& exampleInstructions();
// Fills in descriptions for steps (used for cloud plans too).
std::string describeStep(const PlanStep& step);

// Analysis results a plan needs before it can be applied.
struct PlanInputs {
    std::map<MediaId, SilenceResult> silences;  // remove_silence
    std::map<MediaId, BeatResult> beats;        // add_beat_markers
    std::map<MediaId, SceneResult> scenes;      // split_at_scenes
    std::set<ClipId> selection;
    Time playhead;
};
struct PlanNeeds {
    std::set<MediaId> silence, beats, scenes;
    [[nodiscard]] bool empty() const { return silence.empty() && beats.empty() && scenes.empty(); }
};
PlanNeeds analysisNeeded(const Plan& plan, const Project& project, SequenceId seq, const std::set<ClipId>& selection);

struct ApplyReport {
    std::vector<std::string> log;      // one line per step
    std::vector<std::string> exports;  // export presets requested by the plan
    Time durationBefore, durationAfter;
    size_t clipsBefore = 0, clipsAfter = 0;
};
// Applies every step to the sequence. Fails (and the caller discards the
// editor) if any step fails, so partial plans are never committed.
Status applyPlan(ProjectEditor& pe, SequenceId seq, const Plan& plan, const PlanInputs& in, ApplyReport* report);

// Compact, media-free description of the timeline for the cloud assistant:
// tracks, clip names/times, markers, duration. No file paths or media data.
Json describeTimeline(const Project& project, SequenceId seq, Time playhead, const std::set<ClipId>& selection);

}  // namespace avc::ai
