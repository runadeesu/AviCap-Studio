#pragma once
// AI tools of the editor: media analysis (silence, beats, scenes, highlights)
// on background threads with an on-disk result cache, and the natural-language
// editing workflow Intent -> Plan -> Preview -> Apply.
//
// Everything runs locally. The plan preview is applied to a scratch copy of
// the project (shown in the viewer and timeline, playable) and is committed
// as one undoable edit only when the user presses Apply.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ai/analysis.h"
#include "ai/plan.h"
#include "cache/cache.h"
#include "core/jobs.h"

namespace avc::ui {

class App;

enum class AnalysisKind { Silence, Beats, Scenes, Highlights };
const char* analysisKindLabel(AnalysisKind k);  // English, pass through tr()

// Shaded ranges drawn over the timeline (e.g. the silence that would be cut).
struct TimelineOverlay {
    std::vector<TimeRange> ranges;  // timeline time
    uint32_t color = 0x500000FF;    // ImGui ABGR
    std::string label;
};

class AiService {
public:
    AiService(App& app, cache::CacheStore* store);
    ~AiService();
    AiService(const AiService&) = delete;
    AiService& operator=(const AiService&) = delete;

    // ------------------------------------------------------------ analysis
    ai::SilenceOptions silenceOptions;
    ai::SceneOptions sceneOptions;
    int highlightCount = 5;
    double highlightWindowSec = 4.0;

    // Analyses every media id that has no result yet for the current options.
    // `done(ok, message)` runs on the UI thread.
    void analyze(AnalysisKind kind, std::set<MediaId> media, std::function<void(bool, const std::string&)> done = {});
    void cancel();
    [[nodiscard]] bool busy() const { return job_.valid() && !job_.finished(); }
    [[nodiscard]] float progress() const { return job_.progress(); }
    [[nodiscard]] const std::string& busyLabel() const { return busyLabel_; }

    const ai::SilenceResult* silence(MediaId id) const;
    const ai::BeatResult* beats(MediaId id) const;
    const ai::SceneResult* scenes(MediaId id) const;
    const std::vector<ai::Highlight>* highlights(MediaId id) const;
    void forget(MediaId id);  // media relinked / removed

    // Media used by the tools.
    [[nodiscard]] std::set<MediaId> voiceMedia() const;  // dialogue track of the active sequence
    [[nodiscard]] std::optional<MediaId> musicMedia() const;
    [[nodiscard]] std::set<MediaId> videoMedia(bool selectionOnly) const;

    // Silence that removeSilence() would cut, in timeline time.
    [[nodiscard]] std::vector<TimeRange> silenceRangesOnTimeline() const;
    Status removeSilence();                     // ripple, one undo step
    Status markSilence();                       // adds Silence markers instead of cutting
    Status addBeatMarkers();
    Status addSceneMarkers(bool split);
    Status addHighlightMarkers();

    // ------------------------------------------------------------ plan workflow
    struct PlanState {
        std::string prompt;
        ai::Plan plan;
        std::vector<bool> enabled;  // per step, user can untick steps
        std::string error;
        ai::ApplyReport report;
        bool previewing = false;
    };
    PlanState& planState() { return plan_; }
    void makePlan(const std::string& prompt);
    // Applies the enabled steps to a scratch copy (runs needed analysis first).
    void previewPlan();
    void applyPlan();   // commits as one undo step; queues exports
    void discardPlan();
    [[nodiscard]] ai::Plan enabledPlan() const;

    // Shades the silence that would be cut on the timeline (kept up to date
    // with edits). Returns nullptr when nothing should be drawn.
    bool showSilenceOverlay = false;
    const TimelineOverlay* overlay();

private:
    std::string cacheKey(AnalysisKind kind, const MediaItem& m) const;
    void runPlanPreview();
    ai::PlanInputs planInputs() const;

    App& app_;
    cache::CacheStore* store_;
    JobHandle job_;
    std::string busyLabel_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::map<std::string, ai::SilenceResult> silence_;  // keyed by cacheKey
    std::map<std::string, ai::BeatResult> beats_;
    std::map<std::string, ai::SceneResult> scenes_;
    std::map<std::string, std::vector<ai::Highlight>> highlights_;
    PlanState plan_;
    TimelineOverlay overlay_;
    uint64_t overlayRevision_ = ~0ull;
    size_t overlayResults_ = ~size_t{0};
    std::string overlayKey_;
};

}  // namespace avc::ui
