// UI tests: the complete editor runs headless (real ImGui frames, real panels,
// synthesized mouse/keyboard input) on the CPU render backend.

#include <doctest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/file_io.h"
#include "core/strings.h"
#include "export/verify.h"
#include "tests/test_support.h"
#include "ui/app.h"
#include "ui/commands.h"
#include "ui/headless.h"

using namespace avc;
using namespace avc::ui;

namespace {

class ScriptedDialogs final : public IDialogs {
public:
    std::deque<std::vector<std::string>> open;
    std::deque<std::string> save;
    std::deque<std::string> folders;
    std::vector<std::string> openFiles(const std::string&, const std::vector<FileFilter>&, bool) override {
        if (open.empty()) return {};
        auto r = open.front();
        open.pop_front();
        return r;
    }
    std::optional<std::string> saveFile(const std::string&, const std::vector<FileFilter>&, const std::string&, const std::string&) override {
        if (save.empty()) return std::nullopt;
        auto r = save.front();
        save.pop_front();
        return r;
    }
    std::optional<std::string> pickFolder(const std::string&) override {
        if (folders.empty()) return std::nullopt;
        auto r = folders.front();
        folders.pop_front();
        return r;
    }
};

bool runUntil(HeadlessHost& host, const std::function<bool()>& cond, double timeoutSec = 60.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(timeoutSec * 1000));
    while (std::chrono::steady_clock::now() < end) {
        host.frame();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

ImVec2 clipCenter(HeadlessHost& host, App& app, ClipId id) {
    const Sequence* s = app.sequence();
    const auto ref = s->findClip(id);
    const Clip& c = *s->clip(id);
    const auto* lane = host.window().timeline().laneForTrack(ref.track);
    REQUIRE(lane);
    const float x = host.window().timeline().timeToX(c.start + Time{c.duration.ticks / 2});
    return ImVec2(x, lane->y + lane->h * 0.6f);
}

}  // namespace

TEST_CASE("key chords parse, format and detect conflicts") {
    CHECK(KeyChord::parse("Ctrl+Shift+K").toString() == "Ctrl+Shift+K");
    CHECK(KeyChord::parse("ctrl+z") == KeyChord{ImGuiKey_Z, true, false, false});
    CHECK(KeyChord::parse("Space").key == ImGuiKey_Space);
    CHECK(KeyChord::parse("F5").key == ImGuiKey_F5);
    CHECK(KeyChord::parse("Ctrl+,").key == ImGuiKey_Comma);
    CHECK_FALSE(KeyChord::parse("Hyper+K").valid());
    CHECK_FALSE(KeyChord::parse("").valid());

    CommandRegistry reg;
    int a = 0, b = 0;
    reg.add({"x.a", "A", "Test", {{"avicap", "Ctrl+K"}, {"resolve", "Ctrl+B"}}, [&] { ++a; }, {}, false});
    reg.add({"x.b", "B", "Test", {{"avicap", "B"}}, [&] { ++b; }, [] { return false; }, false});
    CHECK(reg.shortcutText("x.a") == "Ctrl+K");
    reg.setPreset("resolve");
    CHECK(reg.shortcutText("x.a") == "Ctrl+B");
    CHECK(reg.shortcutText("x.b") == "B");  // falls back to the avicap default
    reg.setOverride("x.a", KeyChord::parse("B"));
    CHECK(reg.conflicts(KeyChord::parse("B"), "x.a") == std::vector<std::string>{"x.b"});
    auto saved = reg.saveOverrides();
    CHECK(saved.at("x.a") == "B");
    reg.resetAll();
    CHECK(reg.shortcutText("x.a") == "Ctrl+B");
    reg.loadOverrides(saved);
    CHECK(reg.shortcutText("x.a") == "B");
    CHECK(reg.run("x.a"));
    CHECK_FALSE(reg.run("x.b"));  // disabled
    CHECK(a == 1);
    CHECK(b == 0);
}

TEST_CASE("editor UI golden path (headless)") {
    const std::string video = test::testMedia("av_1080p30.mp4");
    const std::string voice = test::testMedia("speech_silence.wav");
    if (video.empty() || voice.empty()) {
        MESSAGE("test media missing");
        return;
    }
    auto dir = test::makeTempDir("ui_golden");
    auto dialogs = std::make_unique<ScriptedDialogs>();
    ScriptedDialogs* dlg = dialogs.get();
    AppOptions opt;
    opt.dataDir = dir / "data";
    opt.headless = true;
    opt.dialogs = std::move(dialogs);
    opt.loadSettings = false;
    App app(std::move(opt));
    app.snapping = false;  // deterministic drag deltas
    HeadlessHost host(app, ImVec2(1600, 900));
    host.frames(3);
    CHECK(host.drawCalls() > 0);

    // ---- import through the File > Import command (scripted file dialog)
    dlg->open.push_back({video, voice});
    CHECK(app.commands().run("file.import"));
    REQUIRE(runUntil(host, [&] { return app.project().media.size() == 2 && app.importsInProgress() == 0; }));
    const Sequence* seq = app.sequence();
    REQUIRE(seq);
    CHECK(seq->width == 1920);  // the empty sequence adopted the first video's format
    CHECK(seq->height == 1080);
    MediaId videoId = kInvalidId, voiceId = kInvalidId;
    for (const auto& m : app.project().media) (m->info.hasVideo() ? videoId : voiceId) = m->id;

    // ---- put the clip on the timeline (double-click behaviour) and the voice on A2
    app.appendMediaAtPlayhead(videoId);
    REQUIRE(app.sequence()->clipCount() == 2);  // linked video + audio
    const int a2 = app.sequence()->audioTrackIndices().at(1);
    REQUIRE(app.addMediaToTimeline(voiceId, Time::fromSeconds(1.0), -1, a2, false));
    app.selection.clear();
    host.window().timeline().setZoom(150.0, 0.0);  // 150 px per second
    host.frames(3);

    const int v1 = app.sequence()->visualTrackIndices().at(0);
    auto v1Clips = [&] { return app.sequence()->tracks[static_cast<size_t>(v1)]->clips; };
    REQUIRE(v1Clips().size() == 1);
    const ClipId first = v1Clips()[0]->id;

    // ---- click selects the clip and its linked audio
    host.click(clipCenter(host, app, first));
    CHECK(app.selection.clips.size() == 2);

    // ---- razor tool: split V1 (and the linked audio) at 2 s
    app.tool = Tool::Razor;
    const auto* lane = host.window().timeline().laneForTrack(v1);
    REQUIRE(lane);
    host.click(ImVec2(host.window().timeline().timeToX(Time::fromSeconds(2.0)), lane->y + lane->h * 0.6f));
    REQUIRE(v1Clips().size() == 2);
    CHECK(v1Clips()[0]->duration == Time::fromSeconds(2.0));
    app.tool = Tool::Select;

    // ---- drag the right part 1 s to the right (move with live preview, one undo step)
    const ClipId right = v1Clips()[1]->id;
    const ImVec2 from = clipCenter(host, app, right);
    host.drag(from, ImVec2(from.x + 150.0f, from.y));
    REQUIRE(app.sequence()->clip(right));
    CHECK(app.sequence()->clip(right)->start.seconds() == doctest::Approx(3.0).epsilon(0.02));
    CHECK(app.doc().undoLabel() == "Move");

    // ---- trim the tail of the left clip from 2.0 s to 1.5 s
    const ClipId left = v1Clips()[0]->id;
    host.drag(ImVec2(host.window().timeline().timeToX(Time::fromSeconds(2.0)) - 2, lane->y + lane->h * 0.6f),
              ImVec2(host.window().timeline().timeToX(Time::fromSeconds(1.5)) - 2, lane->y + lane->h * 0.6f));
    CHECK(app.sequence()->clip(left)->duration.seconds() == doctest::Approx(1.5).epsilon(0.03));

    // ---- Ctrl+Z restores, Ctrl+Shift+Z re-applies
    host.press(ImGuiKey_Z, true);
    CHECK(app.sequence()->clip(left)->duration.seconds() == doctest::Approx(2.0).epsilon(0.01));
    host.press(ImGuiKey_Z, true, true);
    CHECK(app.sequence()->clip(left)->duration.seconds() == doctest::Approx(1.5).epsilon(0.03));

    // ---- keyboard: Home, then Right x3 steps 3 frames; Ctrl+T adds a title at the playhead
    host.press(ImGuiKey_Home);
    for (int i = 0; i < 3; ++i) host.press(ImGuiKey_RightArrow);
    CHECK(app.playhead() == Time::fromFrames(3, app.sequence()->frameRate));
    host.press(ImGuiKey_T, true);
    const Clip* title = app.primaryClip();
    REQUIRE(title);
    CHECK(title->kind == ClipKind::Text);

    // ---- subtitles, effect and colour on the video clip
    app.addSubtitleClip("こんにちは AviCap", Time::fromSeconds(0.5), Time::fromSeconds(2.0));
    app.selection.clips = {left};
    app.applyEffect("color.basic");
    app.applyEffect("stylize.vignette");
    CHECK(app.sequence()->clip(left)->effects.size() == 2);
    app.applyTransition("cross-dissolve", Time::fromSeconds(0.5));

    // ---- every panel renders without errors
    UiState& ui = app.ui();
    ui.showScopes = ui.showExport = ui.showHistory = ui.showMarkers = ui.showDiagnostics = true;
    ui.showSettings = true;
    app.addMarker();
    host.frames(10);
    ui.showSettings = false;
    ui.showCommandPalette = true;
    host.frames(3);
    host.press(ImGuiKey_Escape);
    ui.fullscreenViewer = true;
    host.frames(3);
    ui.fullscreenViewer = false;
    host.frames(3);

    // ---- preview renders the current frame on the preview thread
    app.seek(Time::fromSeconds(1.0));
    REQUIRE(runUntil(host, [&] {
        const auto f = app.preview().latest();
        return f.texture && f.time == Time::fromSeconds(1.0).snappedToFrame(app.sequence()->frameRate, Rounding::Down);
    }));

    // ---- save, then export through the queue and verify
    const std::string projPath = pathToUtf8(dir / "ui_golden.avicap");
    bool saved = false;
    app.saveProjectAs(projPath, [&](bool ok) { saved = ok; });
    REQUIRE(runUntil(host, [&] { return saved; }));
    CHECK_FALSE(app.doc().dirty());
    CHECK(app.project().name == "ui_golden");

    exp::ExportSettings es = app.defaultExportSettings();
    es.outputPath = pathToUtf8(dir / "ui_golden.mp4");
    es.width = 640;
    es.height = 360;
    es.range = TimeRange{Time{0}, Time::fromSeconds(2.0)};
    if (!enc::EncoderCatalog::instance().best(es.videoCodec, false)) es.videoCodec = enc::VideoCodec::MPEG4;
    auto job = app.queueExport(es);
    REQUIRE(job);
    REQUIRE(runUntil(host, [&] {
        const auto s = job->progress().state;
        return s == exp::ExportState::Done || s == exp::ExportState::Failed;
    }, 180.0));
    CHECK_MESSAGE(job->progress().state == exp::ExportState::Done, job->progress().message);
    CHECK_MESSAGE(job->report().ok, job->report().summary());

    // ---- reopen the saved project
    const size_t clips = app.sequence()->clipCount();
    app.newProject("Scratch", ProjectSettings{});
    CHECK(app.sequence()->clipCount() == 0);
    app.openProject(projPath);
    REQUIRE(runUntil(host, [&] { return !app.busyLoading() && app.sequence() && app.sequence()->clipCount() == clips; }));
    CHECK(app.projectPath() == projPath);
    host.frames(5);
}

TEST_CASE("unsaved-changes guard and crash recovery through the UI") {
    auto dir = test::makeTempDir("ui_recovery");
    {
        AppOptions opt;
        opt.dataDir = dir;
        opt.headless = true;
        opt.loadSettings = false;
        App app(std::move(opt));
        HeadlessHost host(app);
        host.frames(2);
        app.addTextClip("Recover me");
        app.autosave().flush();
        // Simulate a crash: the journal stays because the App is not torn down cleanly.
        // (Copy the autosave directory before the clean shutdown removes it.)
        std::filesystem::copy(dir / "Autosave", dir / "Autosave.crash", std::filesystem::copy_options::recursive);
        bool ran = false;
        app.guardUnsaved([&] { ran = true; });
        CHECK_FALSE(ran);
        CHECK(app.ui().unsavedPrompt);
        app.ui().unsavedPrompt = false;
    }
    std::filesystem::remove_all(dir / "Autosave");
    std::filesystem::rename(dir / "Autosave.crash", dir / "Autosave");
    // The "crashed" instance was this test process: mark its journal as orphaned.
    for (const auto& e : std::filesystem::directory_iterator(dir / "Autosave")) {
        const std::string name = pathToUtf8(e.path().filename());
        if (name.size() < 10 || name.substr(name.size() - 10) != ".meta.json") continue;
        auto j = nlohmann::json::parse(*readFileBytes(e.path()));
        j["pid"] = 0;
        REQUIRE(writeFileAtomic(e.path(), j.dump()));
    }
    AppOptions opt;
    opt.dataDir = dir;
    opt.headless = true;
    opt.loadSettings = false;
    App app(std::move(opt));
    HeadlessHost host(app);
    REQUIRE(app.recoveryCandidates().size() == 1);
    CHECK(app.ui().showRecovery);
    app.recoverProject(app.recoveryCandidates().front());
    REQUIRE(runUntil(host, [&] { return !app.busyLoading() && app.sequence() && app.sequence()->clipCount() == 1; }));
    bool found = false;
    for (const auto& t : app.sequence()->tracks)
        for (const auto& c : t->clips) found |= c->text == "Recover me";
    CHECK(found);
    CHECK(app.doc().dirty());
}

TEST_CASE("AI tools: silence removal and plan preview through the app") {
    const std::string voice = test::testMedia("speech_silence.wav");
    if (voice.empty()) {
        MESSAGE("test media missing");
        return;
    }
    auto dir = test::makeTempDir("ui_ai");
    AppOptions opt;
    opt.dataDir = dir / "data";
    opt.headless = true;
    opt.loadSettings = false;
    App app(std::move(opt));
    HeadlessHost host(app, ImVec2(1600, 900));
    app.ui().showAi = true;
    host.frames(2);

    app.importFiles({voice});
    REQUIRE(runUntil(host, [&] { return app.project().media.size() == 1 && app.importsInProgress() == 0; }));
    const MediaId voiceId = app.project().media[0]->id;
    app.appendMediaAtPlayhead(voiceId);
    REQUIRE(app.sequence()->clipCount() == 1);
    const double before = app.sequence()->duration().seconds();
    CHECK(before == doctest::Approx(9.0).epsilon(0.01));

    AiService& ai = app.ai();
    const auto voiceSet = ai.voiceMedia();
    REQUIRE(voiceSet.count(voiceId) == 1);
    bool done = false, ok = false;
    ai.analyze(AnalysisKind::Silence, voiceSet, [&](bool o, const std::string&) {
        done = true;
        ok = o;
    });
    CHECK(ai.busy());
    REQUIRE(runUntil(host, [&] { return done; }));
    CHECK(ok);
    const auto ranges = ai.silenceRangesOnTimeline();
    REQUIRE(ranges.size() == 2);  // 1-2.5 s and 6-8 s (the 0.2 s dip is kept)
    double cut = 0;
    for (const auto& r : ranges) cut += r.duration.seconds();
    ai.showSilenceOverlay = true;
    host.frames(2);  // timeline draws the overlay, AI panel shows the result
    REQUIRE(ai.overlay());
    CHECK(ai.overlay()->ranges.size() == 2);

    // Results are cached: a second request finishes immediately.
    bool again = false;
    ai.analyze(AnalysisKind::Silence, voiceSet, [&](bool o, const std::string&) { again = o; });
    CHECK(again);
    CHECK_FALSE(ai.busy());

    const size_t undoBefore = app.doc().undoHistory().size();
    REQUIRE(ai.removeSilence());
    CHECK(app.doc().undoHistory().size() == undoBefore + 1);
    CHECK(app.sequence()->duration().seconds() == doctest::Approx(before - cut).epsilon(0.01));
    CHECK_FALSE(ai.overlay());
    app.undo();
    CHECK(app.sequence()->duration().seconds() == doctest::Approx(before).epsilon(0.01));

    // Natural-language plan: preview does not touch the document; apply is one undo step.
    ai.makePlan("最初の1秒をカットして、2倍速にして");
    auto& st = ai.planState();
    REQUIRE(st.plan.steps.size() == 2);
    st.enabled[1] = false;  // the user unticks a step
    const uint64_t rev = app.doc().revision();
    ai.previewPlan();
    REQUIRE(app.planPreview());
    CHECK(st.previewing);
    CHECK(app.doc().revision() == rev);
    CHECK(app.planPreview()->active()->duration().seconds() == doctest::Approx(before - 1.0).epsilon(0.01));
    host.frames(3);  // viewer, timeline banner and panel draw the preview
    ai.applyPlan();
    CHECK_FALSE(app.planPreview());
    CHECK(app.doc().undoLabel() == "AI Edit");
    CHECK(app.sequence()->duration().seconds() == doctest::Approx(before - 1.0).epsilon(0.01));
    app.undo();
    CHECK(app.sequence()->duration().seconds() == doctest::Approx(before).epsilon(0.01));

    // A plan step that cannot run reports an error and changes nothing.
    app.selection.clear();
    ai.makePlan("2倍速にして");  // nothing selected
    ai.previewPlan();
    CHECK_FALSE(app.planPreview());
    CHECK_FALSE(st.error.empty());
    host.frames(2);
}

TEST_CASE("sound library: scan, preview, legal import and timeline placement") {
    const std::string sfx = test::testMedia("beats_120bpm.wav");
    const std::string bgm = test::testMedia("mono_44k.mp3");
    if (sfx.empty() || bgm.empty()) {
        MESSAGE("test media missing");
        return;
    }
    namespace fs = std::filesystem;
    auto dir = test::makeTempDir("ui_sounds");
    const fs::path root = dir / "library";
    const fs::path extra = dir / "my sounds";
    fs::create_directories(root / "BGM");
    fs::create_directories(extra);
    fs::copy_file(pathFromUtf8(bgm), root / "BGM" / "theme.mp3");
    fs::copy_file(pathFromUtf8(sfx), extra / "クリック.wav");

    AppOptions opt;
    opt.dataDir = dir / "data";
    opt.headless = true;
    opt.loadSettings = false;
    App app(std::move(opt));
    app.settings().sounds.rootFolder = pathToUtf8(root);
    app.settings().sounds.libraryFolders = {pathToUtf8(extra)};
    HeadlessHost host(app, ImVec2(1400, 850));
    app.ui().showSounds = true;

    SoundLibrary& lib = app.sounds();
    lib.rescan();
    REQUIRE(runUntil(host, [&] { return !lib.scanning() && lib.items().size() == 2; }));
    const SoundItem* theme = nullptr;
    const SoundItem* click = nullptr;
    for (const auto& s : lib.items()) (s.name == "theme" ? theme : click) = &s;
    REQUIRE(theme);
    REQUIRE(click);
    CHECK(theme->category == "bgm");  // from the folder name
    CHECK(click->category == "sfx");  // short file
    CHECK(click->durationSec == doctest::Approx(8.0).epsilon(0.05));
    const std::string themePath = theme->path, clickPath = click->path;  // items() is replaced by rescans

    // Preview plays on its own output (null device in headless mode) and ends by itself.
    lib.previewer().play(clickPath, 0.5);
    CHECK(lib.previewer().playing());
    REQUIRE(runUntil(host, [&] { return !lib.previewer().playing(); }, 10.0));
    CHECK(lib.previewer().lastError().empty());

    // Favourites and recent are remembered in the settings.
    lib.setFavorite(clickPath, true);
    CHECK(lib.isFavorite(clickPath));

    // Importing a downloaded file copies it; the original stays untouched.
    const fs::path downloaded = dir / "downloaded-sound.wav";
    fs::copy_file(pathFromUtf8(sfx), downloaded);
    const auto sizeBefore = fs::file_size(downloaded);
    auto imported = lib.importFile(pathToUtf8(downloaded), false, "Myinstants", "https://www.myinstants.com/en/instant/example/");
    REQUIRE(imported);
    CHECK(fs::exists(downloaded));
    CHECK(fs::file_size(downloaded) == sizeBefore);
    CHECK(pathFromUtf8(*imported).parent_path() == root / "SFX" / "Myinstants");
    CHECK_FALSE(lib.importFile(pathToUtf8(dir / "missing.wav"), false, {}, {}));
    lib.rescan();
    REQUIRE(runUntil(host, [&] { return !lib.scanning() && lib.items().size() == 3; }));
    const SoundItem* im = lib.find(*imported);
    REQUIRE(im);
    CHECK(im->source == "Myinstants");
    CHECK(im->sourceUrl.find("myinstants.com") != std::string::npos);

    // Search in the browser is a plain URL; nothing is fetched by the app.
    CHECK(SoundLibrary::myinstantsUrl("") == "https://www.myinstants.com/");
    CHECK(SoundLibrary::myinstantsUrl("拍手 sound") == "https://www.myinstants.com/en/search/?name=%E6%8B%8D%E6%89%8B%20sound");

    // Adding to the timeline: music goes to A2, effects to A3 (tracks are created).
    app.addSoundFiles({themePath}, true, Time::fromSeconds(0));
    REQUIRE(runUntil(host, [&] { return app.sequence()->clipCount() == 1; }));
    app.addSoundFiles({clickPath}, false, Time::fromSeconds(2));
    REQUIRE(runUntil(host, [&] { return app.sequence()->clipCount() == 2; }));
    const auto audio = app.sequence()->audioTrackIndices();
    REQUIRE(audio.size() >= 3);
    CHECK(app.sequence()->tracks[static_cast<size_t>(audio[1])]->clips.size() == 1);
    CHECK(app.sequence()->tracks[static_cast<size_t>(audio[2])]->clips.size() == 1);
    host.frames(3);  // the panel draws the library
}

TEST_CASE("AI tools: cloud assistant needs consent and goes through preview/apply") {
    const std::string video = test::testMedia("av_1080p30.mp4");
    if (video.empty()) {
        MESSAGE("test media missing");
        return;
    }
    auto dir = test::makeTempDir("ui_cloud");
    AppOptions opt;
    opt.dataDir = dir / "data";
    opt.headless = true;
    opt.loadSettings = false;
    App app(std::move(opt));
    HeadlessHost host(app, ImVec2(1500, 900));
    app.importFiles({video});
    REQUIRE(runUntil(host, [&] { return app.project().media.size() == 1 && app.importsInProgress() == 0; }));
    app.appendMediaAtPlayhead(app.project().media[0]->id);
    const double before = app.sequence()->duration().seconds();

    AiService& ai = app.ai();
    int calls = 0;
    std::string sentBody;
    ai.transportAvailable = true;
    ai.transport = [&](const HttpRequest& r, const CancelToken&) {
        ++calls;
        sentBody = r.body;
        const nlohmann::json plan = {{"steps", nlohmann::json::array({{{"op", "delete_range"}, {"start", 0.0}, {"end", 1.0}}})},
                                     {"notes", nlohmann::json::array()}};
        const nlohmann::json resp = {{"model", "claude-opus-5-5"},
                                     {"stop_reason", "end_turn"},
                                     {"content", nlohmann::json::array({{{"type", "text"}, {"text", plan.dump()}}})}};
        return HttpResponse{200, resp.dump(), {}};
    };
    app.settings().ai.assistantProvider = "anthropic";
    app.settings().ai.cloudConsent = false;
    app.settings().privacy.allowNetwork = true;
    app.ui().showAi = true;
    host.frames(2);  // the consent box is drawn

    // Without consent nothing is sent.
    CHECK_FALSE(ai.useCloud());
    ai.makeCloudPlan("最初の1秒を消して");
    host.frames(2);
    CHECK(calls == 0);
    CHECK_FALSE(ai.planState().error.empty());

    app.settings().ai.cloudConsent = true;
    if (!ai.apiKeyFromEnvironment()) {
        CHECK_FALSE(ai.useCloud());  // no key yet
        REQUIRE(ai.setApiKey("  test-key\n", false));
    }
    CHECK(ai.useCloud());
    ai.makeCloudPlan("最初の1秒を消して");
    REQUIRE(runUntil(host, [&] { return !ai.cloudBusy() && !ai.planState().plan.steps.empty(); }));
    CHECK(calls == 1);
    CHECK(ai.planState().plan.source == "claude-opus-5-5");
    // The request never contains file paths.
    CHECK(sentBody.find(pathToUtf8(pathFromUtf8(video).parent_path())) == std::string::npos);
    CHECK(sentBody.find("av_1080p30.mp4") != std::string::npos);

    ai.previewPlan();
    REQUIRE(app.planPreview());
    CHECK(app.sequence()->duration().seconds() == doctest::Approx(before));
    ai.applyPlan();
    CHECK(app.sequence()->duration().seconds() == doctest::Approx(before - 1.0).epsilon(0.01));
    CHECK(app.doc().undoLabel() == "AI Edit");
    host.frames(2);
}
