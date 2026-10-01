#include "ui/ai_service.h"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "core/i18n.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "export/export_settings.h"
#include "ui/app.h"

namespace avc::ui {

namespace fs = std::filesystem;
using Json = nlohmann::json;

const char* analysisKindLabel(AnalysisKind k) {
    switch (k) {
        case AnalysisKind::Silence: return trNoop("Detecting silence");
        case AnalysisKind::Beats: return trNoop("Detecting beats");
        case AnalysisKind::Scenes: return trNoop("Detecting scene changes");
        case AnalysisKind::Highlights: return trNoop("Finding highlights");
    }
    return trNoop("Analysing");
}

namespace {

// ------------------------------------------------------------------ result (de)serialisation

Json rangesToJson(const std::vector<TimeRange>& v) {
    Json a = Json::array();
    for (const auto& r : v) a.push_back({r.start.ticks, r.duration.ticks});
    return a;
}
std::vector<TimeRange> rangesFromJson(const Json& a) {
    std::vector<TimeRange> v;
    if (a.is_array())
        for (const auto& r : a)
            if (r.is_array() && r.size() == 2) v.emplace_back(Time{r[0].get<int64_t>()}, Time{r[1].get<int64_t>()});
    return v;
}
Json timesToJson(const std::vector<Time>& v) {
    Json a = Json::array();
    for (Time t : v) a.push_back(t.ticks);
    return a;
}
std::vector<Time> timesFromJson(const Json& a) {
    std::vector<Time> v;
    if (a.is_array())
        for (const auto& t : a) v.emplace_back(t.get<int64_t>());
    return v;
}

Json toJson(const ai::SilenceResult& r) {
    return {{"silences", rangesToJson(r.silences)}, {"threshold", r.thresholdDb}, {"floor", r.noiseFloorDb}, {"duration", r.durationSec}};
}
Json toJson(const ai::BeatResult& r) { return {{"bpm", r.bpm}, {"beats", timesToJson(r.beats)}, {"confidence", r.confidence}}; }
Json toJson(const ai::SceneResult& r) { return {{"cuts", timesToJson(r.cuts)}, {"scores", r.scores}}; }
Json toJson(const std::vector<ai::Highlight>& v) {
    Json a = Json::array();
    for (const auto& h : v) a.push_back({{"start", h.range.start.ticks}, {"duration", h.range.duration.ticks}, {"score", h.score}});
    return a;
}

template <class T>
std::optional<T> fromJson(const std::string& text);

template <>
std::optional<ai::SilenceResult> fromJson(const std::string& text) {
    Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    ai::SilenceResult r;
    r.silences = rangesFromJson(j.value("silences", Json::array()));
    r.thresholdDb = j.value("threshold", 0.0f);
    r.noiseFloorDb = j.value("floor", 0.0f);
    r.durationSec = j.value("duration", 0.0);
    return r;
}
template <>
std::optional<ai::BeatResult> fromJson(const std::string& text) {
    Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    ai::BeatResult r;
    r.bpm = j.value("bpm", 0.0);
    r.beats = timesFromJson(j.value("beats", Json::array()));
    r.confidence = j.value("confidence", 0.0f);
    return r;
}
template <>
std::optional<ai::SceneResult> fromJson(const std::string& text) {
    Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    ai::SceneResult r;
    r.cuts = timesFromJson(j.value("cuts", Json::array()));
    if (j.contains("scores") && j["scores"].is_array()) r.scores = j["scores"].get<std::vector<float>>();
    return r;
}
template <>
std::optional<std::vector<ai::Highlight>> fromJson(const std::string& text) {
    Json j = Json::parse(text, nullptr, false);
    if (!j.is_array()) return std::nullopt;
    std::vector<ai::Highlight> v;
    for (const auto& h : j) {
        if (!h.is_object()) continue;
        ai::Highlight x;
        x.range = {Time{h.value("start", int64_t{0})}, Time{h.value("duration", int64_t{0})}};
        x.score = h.value("score", 0.0f);
        v.push_back(x);
    }
    return v;
}

const char* kindId(AnalysisKind k) {
    switch (k) {
        case AnalysisKind::Silence: return "silence";
        case AnalysisKind::Beats: return "beats";
        case AnalysisKind::Scenes: return "scenes";
        case AnalysisKind::Highlights: return "highlights";
    }
    return "x";
}

}  // namespace

AiService::AiService(App& app, cache::CacheStore* store) : app_(app), store_(store) {}

AiService::~AiService() {
    *alive_ = false;
    job_.cancel();
    cloudJob_.cancel();
    job_.wait();
    cloudJob_.wait();
}

std::string AiService::cacheKey(AnalysisKind kind, const MediaItem& m) const {
    char opts[160] = {};
    switch (kind) {
        case AnalysisKind::Silence:
            std::snprintf(opts, sizeof opts, "a%d_t%.1f_m%.2f_p%.2f", silenceOptions.autoThreshold ? 1 : 0, silenceOptions.thresholdDb,
                          silenceOptions.minSilenceSec, silenceOptions.paddingSec);
            break;
        case AnalysisKind::Scenes:
            std::snprintf(opts, sizeof opts, "t%.2f_m%.2f_w%d", sceneOptions.threshold, sceneOptions.minSceneSec, sceneOptions.analysisWidth);
            break;
        case AnalysisKind::Highlights: std::snprintf(opts, sizeof opts, "n%d_w%.1f", highlightCount, highlightWindowSec); break;
        case AnalysisKind::Beats: std::snprintf(opts, sizeof opts, "v1"); break;
    }
    return cache::mediaKey(m) + "_" + kindId(kind) + "_" + opts;
}

void AiService::analyze(AnalysisKind kind, std::set<MediaId> media, std::function<void(bool, const std::string&)> done) {
    if (busy()) {
        if (done) done(false, tr("Another analysis is running"));
        return;
    }
    struct Item {
        MediaId id;
        std::string path;
        std::string key;
        std::string name;
    };
    std::vector<Item> todo;
    for (MediaId id : media) {
        const MediaItem* m = app_.project().findMedia(id);
        if (!m) continue;
        const std::string key = cacheKey(kind, *m);
        const bool have = (kind == AnalysisKind::Silence && silence_.count(key)) || (kind == AnalysisKind::Beats && beats_.count(key)) ||
                          (kind == AnalysisKind::Scenes && scenes_.count(key)) ||
                          (kind == AnalysisKind::Highlights && highlights_.count(key));
        if (have) continue;
        if (kind != AnalysisKind::Scenes && !m->info.hasAudio()) continue;
        if (kind == AnalysisKind::Scenes && !m->info.hasVideo()) continue;
        todo.push_back({id, m->path, key, m->name});
    }
    if (todo.empty()) {
        if (done) done(true, {});
        return;
    }
    busyLabel_ = tr(analysisKindLabel(kind));
    const ai::SilenceOptions sopt = silenceOptions;
    const ai::SceneOptions scopt = sceneOptions;
    const int hcount = highlightCount;
    const double hwin = highlightWindowSec;
    cache::CacheStore* store = store_;
    std::weak_ptr<bool> alive = alive_;
    App* app = &app_;
    job_ = Jobs::analysis().submit(
        busyLabel_, JobPriority::Background,
        [=, this](JobContext& ctx) {
            struct Out {
                std::string key;
                std::string json;
            };
            auto outs = std::make_shared<std::vector<Out>>();
            std::string error;
            for (size_t i = 0; i < todo.size() && !ctx.cancelled(); ++i) {
                const Item& it = todo[i];
                ctx.setMessage(it.name + " (" + std::to_string(i + 1) + "/" + std::to_string(todo.size()) + ")");
                if (store)
                    if (auto cached = store->read("analysis", it.key, ".json")) {
                        outs->push_back({it.key, *cached});
                        continue;
                    }
                Json j;
                Status st;
                switch (kind) {
                    case AnalysisKind::Silence: {
                        auto r = ai::detectSilence(it.path, sopt, ctx.token(), &ctx);
                        if (r) j = toJson(*r);
                        else st = r.status();
                        break;
                    }
                    case AnalysisKind::Beats: {
                        auto r = ai::detectBeats(it.path, ctx.token(), &ctx);
                        if (r) j = toJson(*r);
                        else st = r.status();
                        break;
                    }
                    case AnalysisKind::Scenes: {
                        auto r = ai::detectScenes(it.path, scopt, ctx.token(), &ctx);
                        if (r) j = toJson(*r);
                        else st = r.status();
                        break;
                    }
                    case AnalysisKind::Highlights: {
                        auto r = ai::findHighlights(it.path, hcount, hwin, ctx.token(), &ctx);
                        if (r) j = toJson(*r);
                        else st = r.status();
                        break;
                    }
                }
                if (!st) {
                    if (ctx.cancelled()) break;
                    error = it.name + ": " + st.message();
                    AVC_WARN("ai", "{} failed for {}: {}", kindId(kind), it.name, st.message());
                    continue;
                }
                std::string text = j.dump();
                if (store) store->write("analysis", it.key, ".json", text);
                outs->push_back({it.key, std::move(text)});
            }
            const bool cancelled = ctx.cancelled();
            app->post([=, this] {
                if (!alive.lock() || !*alive.lock()) return;
                for (const auto& o : *outs) {
                    switch (kind) {
                        case AnalysisKind::Silence:
                            if (auto r = fromJson<ai::SilenceResult>(o.json)) silence_[o.key] = *r;
                            break;
                        case AnalysisKind::Beats:
                            if (auto r = fromJson<ai::BeatResult>(o.json)) beats_[o.key] = *r;
                            break;
                        case AnalysisKind::Scenes:
                            if (auto r = fromJson<ai::SceneResult>(o.json)) scenes_[o.key] = *r;
                            break;
                        case AnalysisKind::Highlights:
                            if (auto r = fromJson<std::vector<ai::Highlight>>(o.json)) highlights_[o.key] = *r;
                            break;
                    }
                }
                busyLabel_.clear();
                if (cancelled) {
                    if (done) done(false, tr("Cancelled"));
                } else if (done) {
                    done(error.empty(), error);
                }
                app_.wake();
            });
        });
}

void AiService::cancel() { job_.cancel(); }

const ai::SilenceResult* AiService::silence(MediaId id) const {
    const MediaItem* m = app_.project().findMedia(id);
    if (!m) return nullptr;
    auto it = silence_.find(cacheKey(AnalysisKind::Silence, *m));
    return it == silence_.end() ? nullptr : &it->second;
}
const ai::BeatResult* AiService::beats(MediaId id) const {
    const MediaItem* m = app_.project().findMedia(id);
    if (!m) return nullptr;
    auto it = beats_.find(cacheKey(AnalysisKind::Beats, *m));
    return it == beats_.end() ? nullptr : &it->second;
}
const ai::SceneResult* AiService::scenes(MediaId id) const {
    const MediaItem* m = app_.project().findMedia(id);
    if (!m) return nullptr;
    auto it = scenes_.find(cacheKey(AnalysisKind::Scenes, *m));
    return it == scenes_.end() ? nullptr : &it->second;
}
const std::vector<ai::Highlight>* AiService::highlights(MediaId id) const {
    const MediaItem* m = app_.project().findMedia(id);
    if (!m) return nullptr;
    auto it = highlights_.find(cacheKey(AnalysisKind::Highlights, *m));
    return it == highlights_.end() ? nullptr : &it->second;
}

void AiService::forget(MediaId id) {
    const MediaItem* m = app_.project().findMedia(id);
    if (!m) return;
    const std::string prefix = cache::mediaKey(*m) + "_";
    auto drop = [&](auto& map) { std::erase_if(map, [&](const auto& kv) { return kv.first.rfind(prefix, 0) == 0; }); };
    drop(silence_);
    drop(beats_);
    drop(scenes_);
    drop(highlights_);
}

// ------------------------------------------------------------------ media selection

std::set<MediaId> AiService::voiceMedia() const {
    std::set<MediaId> out;
    const Sequence* seq = app_.sequence();
    if (!seq) return out;
    // Same rule as the plan engine: the audio track with the most dialogue.
    const ai::Plan probe{{{"remove_silence", ai::Json::object(), {}}}, "local", {}};
    return ai::analysisNeeded(probe, app_.project(), seq->id, app_.selection.clips).silence;
}

std::optional<MediaId> AiService::musicMedia() const {
    const ai::Plan probe{{{"add_beat_markers", ai::Json::object(), {}}}, "local", {}};
    const Sequence* seq = app_.sequence();
    if (!seq) return std::nullopt;
    auto n = ai::analysisNeeded(probe, app_.project(), seq->id, app_.selection.clips);
    if (n.beats.empty()) return std::nullopt;
    return *n.beats.begin();
}

std::set<MediaId> AiService::videoMedia(bool selectionOnly) const {
    std::set<MediaId> out;
    const Sequence* seq = app_.sequence();
    if (!seq) return out;
    for (const auto& t : seq->tracks) {
        if (t->kind != TrackKind::Video) continue;
        for (const auto& c : t->clips)
            if (c->kind == ClipKind::Media && (!selectionOnly || app_.selection.clips.count(c->id)))
                if (const MediaItem* m = app_.project().findMedia(c->media); m && m->info.hasVideo() && m->info.kind == MediaKind::Video)
                    out.insert(c->media);
    }
    return out;
}

// ------------------------------------------------------------------ direct tools

std::vector<TimeRange> AiService::silenceRangesOnTimeline() const {
    std::vector<TimeRange> out;
    const Sequence* seq = app_.sequence();
    if (!seq) return out;
    const std::set<MediaId> voice = voiceMedia();
    for (const auto& t : seq->tracks) {
        if (isVisualTrack(t->kind)) continue;
        for (const auto& c : t->clips) {
            if (c->kind != ClipKind::Media || !voice.count(c->media)) continue;
            if (const ai::SilenceResult* r = silence(c->media)) {
                auto v = ai::sourceRangesToTimeline(*c, r->silences);
                out.insert(out.end(), v.begin(), v.end());
            }
        }
    }
    return ai::normalizeRanges(out, Time::fromMilliseconds(100));
}

Status AiService::removeSilence() {
    const auto ranges = silenceRangesOnTimeline();
    if (ranges.empty()) return Status::error(tr("No silence found"));
    Time total{0};
    for (const auto& r : ranges) total += r.duration;
    Status st = app_.editSequence(trNoop("Remove Silence"), [&](SequenceEditor& e) { return ai::rippleRemoveRanges(e, ranges); });
    if (st) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%zu / %.1f s", ranges.size(), total.seconds());
        app_.notify(LogLevel::Info, std::string(tr("Silence removed")) + ": " + buf, tr("Undo"), [this] { app_.undo(); });
        showSilenceOverlay = false;
    }
    return st;
}

Status AiService::markSilence() {
    const auto ranges = silenceRangesOnTimeline();
    if (ranges.empty()) return Status::error(tr("No silence found"));
    return app_.editSequence(trNoop("Mark Silence"), [&](SequenceEditor& e) {
        edit::removeMarkersOfKind(e, MarkerKind::Silence);
        for (const auto& r : ranges) {
            Marker m;
            m.time = r.start.snappedToFrame(e.seq().frameRate);
            m.duration = r.duration;
            m.kind = MarkerKind::Silence;
            m.color = 0xFF5050E0;
            m.name = tr("Silence");
            edit::addMarker(e, m);
        }
        return Status::ok();
    });
}

Status AiService::addBeatMarkers() {
    const Sequence* seq = app_.sequence();
    if (!seq) return Status::error("No sequence");
    ai::Plan p{{{"add_beat_markers", ai::Json::object(), {}}}, "local", {}};
    ai::PlanInputs in = planInputs();
    return app_.editProject(trNoop("Add Beat Markers"), [&](ProjectEditor& pe) { return ai::applyPlan(pe, seq->id, p, in, nullptr); });
}

Status AiService::addSceneMarkers(bool split) {
    const Sequence* seq = app_.sequence();
    if (!seq) return Status::error("No sequence");
    if (split) {
        ai::Plan p{{{"split_at_scenes", {{"target", app_.selection.clips.empty() ? "all_video" : "selected"}}, {}}}, "local", {}};
        ai::PlanInputs in = planInputs();
        return app_.editProject(trNoop("Split at Scene Changes"), [&](ProjectEditor& pe) { return ai::applyPlan(pe, seq->id, p, in, nullptr); });
    }
    return app_.editSequence(trNoop("Add Scene Markers"), [&](SequenceEditor& e) {
        edit::removeMarkersOfKind(e, MarkerKind::Scene);
        int n = 0;
        for (const auto& t : e.seq().tracks) {
            if (t->kind != TrackKind::Video) continue;
            for (const auto& c : t->clips) {
                const ai::SceneResult* r = c->kind == ClipKind::Media ? scenes(c->media) : nullptr;
                if (!r) continue;
                for (Time cut : r->cuts)
                    if (auto tl = ai::sourceTimeToTimeline(*c, cut)) {
                        Marker m;
                        m.time = tl->snappedToFrame(e.seq().frameRate);
                        m.kind = MarkerKind::Scene;
                        m.color = 0xFF40E0A0;
                        m.name = tr("Scene");
                        edit::addMarker(e, m);
                        ++n;
                    }
            }
        }
        return n > 0 ? Status::ok() : Status::error(tr("No scene changes found"));
    });
}

Status AiService::addHighlightMarkers() {
    return app_.editSequence(trNoop("Add Highlight Markers"), [&](SequenceEditor& e) {
        edit::removeMarkersOfKind(e, MarkerKind::Highlight);
        int n = 0;
        for (const auto& t : e.seq().tracks) {
            if (isVisualTrack(t->kind)) continue;
            for (const auto& c : t->clips) {
                const auto* hs = c->kind == ClipKind::Media ? highlights(c->media) : nullptr;
                if (!hs) continue;
                for (const auto& h : *hs) {
                    auto v = ai::sourceRangesToTimeline(*c, {h.range});
                    if (v.empty()) continue;
                    Marker m;
                    m.time = v[0].start.snappedToFrame(e.seq().frameRate);
                    m.duration = v[0].duration;
                    m.kind = MarkerKind::Highlight;
                    m.color = 0xFF30C0FF;
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%s +%.1f dB", tr("Highlight"), h.score);
                    m.name = buf;
                    edit::addMarker(e, m);
                    ++n;
                }
            }
        }
        return n > 0 ? Status::ok() : Status::error(tr("No highlights found"));
    });
}

const TimelineOverlay* AiService::overlay() {
    if (!showSilenceOverlay) return nullptr;
    const uint64_t rev = app_.doc().revision();
    char opts[96];
    std::snprintf(opts, sizeof opts, "%d_%.1f_%.2f_%.2f", silenceOptions.autoThreshold ? 1 : 0, silenceOptions.thresholdDb,
                  silenceOptions.minSilenceSec, silenceOptions.paddingSec);
    if (rev != overlayRevision_ || silence_.size() != overlayResults_ || overlayKey_ != opts) {
        overlayRevision_ = rev;
        overlayResults_ = silence_.size();
        overlayKey_ = opts;
        overlay_.ranges = silenceRangesOnTimeline();
        overlay_.color = 0x553C3CE6;  // translucent red (ABGR)
        overlay_.label = tr("Silence");
    }
    return &overlay_;
}

// ------------------------------------------------------------------ plan workflow

ai::PlanInputs AiService::planInputs() const {
    ai::PlanInputs in;
    in.selection = app_.selection.clips;
    in.playhead = app_.playhead();
    for (const auto& m : app_.project().media) {
        if (auto* s = silence(m->id)) in.silences[m->id] = *s;
        if (auto* b = beats(m->id)) in.beats[m->id] = *b;
        if (auto* sc = scenes(m->id)) in.scenes[m->id] = *sc;
    }
    return in;
}

void AiService::makePlan(const std::string& prompt) {
    discardPlan();
    plan_ = {};
    plan_.prompt = prompt;
    const Sequence* seq = app_.sequence();
    ai::PlanContext ctx;
    ctx.playhead = app_.playhead();
    ctx.sequenceDuration = seq ? seq->duration() : Time{0};
    ctx.hasSelection = !app_.selection.clips.empty();
    plan_.plan = ai::planFromText(prompt, ctx);
    plan_.enabled.assign(plan_.plan.steps.size(), true);
    if (plan_.plan.steps.empty()) plan_.error = tr("No editing instructions were recognised. Try one of the examples.");
}

ai::Plan AiService::enabledPlan() const {
    ai::Plan p = plan_.plan;
    p.steps.clear();
    for (size_t i = 0; i < plan_.plan.steps.size(); ++i)
        if (i < plan_.enabled.size() && plan_.enabled[i]) p.steps.push_back(plan_.plan.steps[i]);
    return p;
}

void AiService::previewPlan() {
    plan_.error.clear();
    const Sequence* seq = app_.sequence();
    if (!seq) return;
    const ai::Plan p = enabledPlan();
    if (p.empty()) {
        plan_.error = tr("No steps selected");
        return;
    }
    // Run the analysis the plan needs first (one kind at a time), then preview.
    const ai::PlanNeeds needs = ai::analysisNeeded(p, app_.project(), seq->id, app_.selection.clips);
    auto missing = [&](AnalysisKind k, const std::set<MediaId>& ids) {
        std::set<MediaId> out;
        for (MediaId id : ids)
            if ((k == AnalysisKind::Silence && !silence(id)) || (k == AnalysisKind::Beats && !beats(id)) ||
                (k == AnalysisKind::Scenes && !scenes(id)))
                out.insert(id);
        return out;
    };
    std::weak_ptr<bool> alive = alive_;
    auto next = [this, alive](bool ok, const std::string& msg) {
        if (!alive.lock()) return;
        if (!ok) {
            plan_.error = msg;
            return;
        }
        previewPlan();  // re-enters until nothing is missing
    };
    if (auto m = missing(AnalysisKind::Silence, needs.silence); !m.empty()) return analyze(AnalysisKind::Silence, m, next);
    if (auto m = missing(AnalysisKind::Beats, needs.beats); !m.empty()) return analyze(AnalysisKind::Beats, m, next);
    if (auto m = missing(AnalysisKind::Scenes, needs.scenes); !m.empty()) return analyze(AnalysisKind::Scenes, m, next);
    runPlanPreview();
}

void AiService::runPlanPreview() {
    const Sequence* seq = app_.sequence();
    if (!seq) return;
    ProjectEditor pe(app_.doc().current());
    plan_.report = {};
    const Status st = ai::applyPlan(pe, seq->id, enabledPlan(), planInputs(), &plan_.report);
    if (!st) {
        plan_.error = st.message();
        app_.setPlanPreview(nullptr);
        plan_.previewing = false;
        return;
    }
    app_.setPlanPreview(pe.finish());
    plan_.previewing = true;
}

void AiService::applyPlan() {
    const Sequence* seq = app_.sequence();
    if (!seq) return;
    const ai::Plan p = enabledPlan();
    if (p.empty()) return;
    // Make sure the analysis exists (the user may apply without previewing).
    if (!ai::analysisNeeded(p, app_.project(), seq->id, app_.selection.clips).empty() && !plan_.previewing) {
        previewPlan();
        if (!plan_.previewing) {
            if (plan_.error.empty()) plan_.error = tr("Analysing... press Apply again when the preview is ready");
            return;
        }
    }
    ai::ApplyReport rep;
    const ai::PlanInputs in = planInputs();
    app_.setPlanPreview(nullptr);
    plan_.previewing = false;
    const Status st = app_.editProject(trNoop("AI Edit"), [&](ProjectEditor& pe) { return ai::applyPlan(pe, seq->id, p, in, &rep); });
    if (!st) {
        plan_.error = st.message();
        return;
    }
    plan_.report = rep;
    for (const std::string& preset : rep.exports) {
        exp::ExportSettings s = app_.defaultExportSettings();
        const std::string out = s.outputPath;
        s = exp::applyPreset(preset, s);
        fs::path path = pathFromUtf8(out);
        path.replace_extension(pathFromUtf8(exp::defaultExtension(s.container)));
        s.outputPath = pathToUtf8(path);
        app_.queueExport(s);
    }
    app_.notify(LogLevel::Info, std::string(tr("AI edit applied")) + " (" + std::to_string(p.steps.size()) + ")", tr("Undo"),
                [this] { app_.undo(); });
}

// ------------------------------------------------------------------ cloud assistant

namespace {
fs::path keyFile(const App& app) { return app.dataDir() / "secrets" / "anthropic.key"; }
}  // namespace

std::string AiService::cloudBlocker() const {
    const AppSettings& s = app_.settings();
    if (!transportAvailable) return tr("The cloud assistant is only available in the Windows version.");
    if (!s.privacy.allowNetwork) return tr("Network access is turned off in Settings > AI & Privacy.");
    if (!s.ai.cloudConsent) return tr("Consent is required before anything is sent.");
    if (apiKey().empty()) return tr("Enter an Anthropic API key.");
    return {};
}

bool AiService::useCloud() const { return app_.settings().ai.assistantProvider == "anthropic" && cloudBlocker().empty(); }

bool AiService::apiKeyFromEnvironment() const {
    auto env = getEnv("ANTHROPIC_API_KEY");
    return env && !env->empty();
}

std::string AiService::apiKey() const {
    if (auto env = getEnv("ANTHROPIC_API_KEY"); env && !env->empty()) return *env;
    if (!sessionKey_.empty()) return sessionKey_;
    if (auto k = loadUserSecret(keyFile(app_))) return *k;
    return {};
}

bool AiService::hasStoredKey() const {
    std::error_code ec;
    return fs::exists(keyFile(app_), ec);
}

Status AiService::setApiKey(const std::string& key, bool remember) {
    std::string k = key;
    while (!k.empty() && (k.back() == ' ' || k.back() == '\n' || k.back() == '\r')) k.pop_back();
    while (!k.empty() && k.front() == ' ') k.erase(k.begin());
    if (k.empty()) return Status::error(tr("Enter an Anthropic API key."));
    sessionKey_ = k;
    if (remember) return saveUserSecret(keyFile(app_), k);
    return Status::ok();
}

void AiService::forgetApiKey() {
    sessionKey_.clear();
    std::error_code ec;
    fs::remove(keyFile(app_), ec);
}

void AiService::makeCloudPlan(const std::string& prompt) {
    if (cloudBusy()) return;
    discardPlan();
    plan_ = {};
    plan_.prompt = prompt;
    if (const std::string why = cloudBlocker(); !why.empty()) {
        plan_.error = why;
        return;
    }
    const Sequence* seq = app_.sequence();
    if (!seq) return;
    ai::CloudOptions opt;
    opt.apiKey = apiKey();
    if (!app_.settings().ai.anthropicModel.empty()) opt.model = app_.settings().ai.anthropicModel;
    // Built on the UI thread: names, times and texts only (no paths, no media).
    const ai::Json timeline = ai::describeTimeline(app_.project(), seq->id, app_.playhead(), app_.selection.clips);
    auto transport_ = transport;
    std::weak_ptr<bool> alive = alive_;
    App* app = &app_;
    AVC_INFO("ai", "sending an editing request to the cloud assistant ({})", opt.model);
    cloudJob_ = Jobs::io().submit("Cloud assistant", JobPriority::High, [=, this](JobContext& ctx) {
        auto r = ai::requestCloudPlan(prompt, timeline, opt, transport_, ctx.token());
        app->post([=, this] {
            if (!alive.lock()) return;
            if (!r) {
                plan_.error = std::string(tr("The cloud assistant failed")) + ": " + r.errorMessage();
                AVC_WARN("ai", "cloud assistant failed: {}", r.errorMessage());
                return;
            }
            plan_.plan = *r;
            plan_.enabled.assign(plan_.plan.steps.size(), true);
            if (plan_.plan.steps.empty() && plan_.plan.notes.empty())
                plan_.error = tr("No editing instructions were recognised. Try one of the examples.");
            app_.wake();
        });
    });
}

void AiService::discardPlan() {
    if (plan_.previewing) app_.setPlanPreview(nullptr);
    plan_.previewing = false;
}

}  // namespace avc::ui
