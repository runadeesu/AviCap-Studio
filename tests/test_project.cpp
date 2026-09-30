#include <doctest.h>

#include <thread>

#include "core/file_io.h"
#include "core/strings.h"
#include "project/journal.h"
#include "project/relink.h"
#include "project/serialize.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;
using namespace avc::edit;

namespace {

Time S(double s) { return Time::fromSeconds(s); }

// Canonical JSON for comparisons (drops save-time-only fields).
std::string canonical(const Project& p) {
    Json j = projectToJson(p);
    j.erase("savedUtc");
    for (auto& m : j["project"]["media"]) m.erase("relativePath");
    return j.dump();
}

MediaItem media(const std::string& path, double seconds) {
    MediaItem m;
    m.id = newId();
    m.name = pathToUtf8(pathFromUtf8(path).filename());
    m.path = path;
    m.info.kind = MediaKind::Video;
    m.info.duration = S(seconds);
    VideoStreamInfo v;
    v.index = 0;
    v.width = 1280;
    v.height = 720;
    v.frameRate = {30000, 1001};
    v.codec = "h264";
    m.info.video.push_back(v);
    AudioStreamInfo a;
    a.index = 1;
    a.sampleRate = 48000;
    a.channels = 2;
    m.info.audio.push_back(a);
    return m;
}

// Builds a project exercising most model features.
ProjectPtr richProject() {
    Document doc(std::make_shared<Project>(makeProject("Rich 日本語")));
    const SequenceId sid = doc.project().activeSequence;
    MediaItem m = media("C:/footage/クリップ.mp4", 20);
    const MediaId mid = m.id;
    REQUIRE(doc.edit("import", [&](ProjectEditor& pe) {
        pe.addMedia(m);
        return Status::ok();
    }));
    REQUIRE(doc.editSequence("build", sid, [&](SequenceEditor& e) -> Status {
        auto ids = addMediaClip(e, *doc.project().findMedia(mid), S(0), 0, 3, PlaceMode::Overwrite);
        if (!ids) return ids.status();
        Clip& v = e.mutableClip((*ids)[0]);
        v.transform.getOrAdd("opacity").addKey({S(0), pv(0), Interp::EaseInOut});
        v.transform.getOrAdd("opacity").addKey({S(1), pv(100), Interp::Bezier, 0.1f, 0.2f, 0.3f, 0.9f});
        v.blend = BlendMode::Screen;
        v.speed = {3, 2};
        v.effects.push_back(EffectInstance{newId(), "blur.gaussian", true, {}});
        v.effects.back().params.setStatic("radius", pv(12));
        v.transitionIn = TransitionSpec{"cross-dissolve", S(0.5), {}};
        Clip t = makeTextClip("こんにちは\nWorld", S(2), S(3));
        t.textStyle.fontFamily = "Meiryo";
        t.textParams.setStatic("strokeWidth", pv(3));
        if (auto st = placeClips(e, {{1, t}}, PlaceMode::Overwrite); !st) return st;
        Marker mk;
        mk.time = S(4);
        mk.name = "Beat";
        mk.kind = MarkerKind::Beat;
        mk.score = 0.8f;
        addMarker(e, mk);
        e.props().workArea = TimeRange{S(1), S(5)};
        e.mutableTrack(3).volumeDb = -6;
        return Status::ok();
    }));
    return doc.current();
}

}  // namespace

TEST_CASE("project JSON roundtrip preserves the model exactly") {
    ProjectPtr p = richProject();
    const std::string text = projectToString(*p);
    auto r = projectFromString(text);
    REQUIRE_MESSAGE(r, r.errorMessage());
    CHECK(r->warnings.empty());
    const Project& q = *r->project;
    CHECK(q.name == p->name);
    CHECK(q.media.size() == 1);
    CHECK(*q.media[0] == *p->media[0]);
    REQUIRE(q.sequences.size() == 1);
    const Sequence& a = *p->sequences[0];
    const Sequence& b = *q.sequences[0];
    CHECK(a.frameRate == b.frameRate);
    CHECK(a.markers == b.markers);
    CHECK(a.workArea == b.workArea);
    REQUIRE(a.tracks.size() == b.tracks.size());
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        CHECK(trackPropsToJson(*a.tracks[i]) == trackPropsToJson(*b.tracks[i]));
        REQUIRE(a.tracks[i]->clips.size() == b.tracks[i]->clips.size());
        for (size_t c = 0; c < a.tracks[i]->clips.size(); ++c) CHECK(*a.tracks[i]->clips[c] == *b.tracks[i]->clips[c]);
    }
    // Second roundtrip is byte-identical except the save timestamp.
    Json j1 = projectToJson(*p), j2 = projectToJson(q);
    j1.erase("savedUtc");
    j2.erase("savedUtc");
    CHECK(j1 == j2);
}

TEST_CASE("newer schema is rejected with a clear message") {
    Json j = projectToJson(*richProject());
    j["schemaVersion"] = kProjectSchemaVersion + 5;
    auto r = projectFromJson(j);
    CHECK_FALSE(r);
    CHECK(r.errorMessage().find("newer version") != std::string::npos);
}

TEST_CASE("schema 0 prototype files are migrated") {
    const char* v0 = R"({
      "format": "avicap-project", "schemaVersion": 0,
      "project": {"id": "00000000000000aa", "name": "Old", "settings": {"frameRate": 29.97},
        "media": [{"id": "0000000000000001", "name": "a.mp4", "path": "C:/a.mp4",
                   "info": {"kind": "video", "duration": 10.0, "video": [{"index":0,"width":640,"height":480}]}}],
        "sequences": [{"id": "0000000000000002", "name": "S", "frameRate": 25,
          "tracks": [{"id": "0000000000000003", "kind": "video", "name": "V1",
            "clips": [{"id": "0000000000000004", "kind": "media", "media": "0000000000000001",
                       "start": 1.5, "duration": 2.0, "sourceIn": 0.5}]}]}],
        "activeSequence": "0000000000000002"}})";
    auto r = projectFromString(v0);
    REQUIRE_MESSAGE(r, r.errorMessage());
    CHECK(r->migrated);
    const Sequence& s = *r->project->sequences[0];
    CHECK(s.frameRate == Rational{25, 1});
    CHECK(r->project->settings.frameRate == Rational{30000, 1001});
    const Clip& c = *s.tracks[0]->clips[0];
    CHECK(c.start == S(1.5));
    CHECK(c.duration == S(2));
    CHECK(c.sourceIn == S(0.5));
    CHECK(r->project->media[0]->info.duration == S(10));
}

TEST_CASE("damaged project files are repaired or fall back to backup") {
    auto dir = test::makeTempDir("damaged");
    auto file = dir / "p.avicap";
    ProjectPtr p = richProject();
    REQUIRE(saveProjectFile(*p, file));
    REQUIRE(saveProjectFile(*p, file));  // creates .bak
    // Truncate the main file (simulated crash in an external tool).
    auto text = *readFileBytes(file);
    REQUIRE(writeFileAtomic(file, text.substr(0, text.size() / 2)));
    auto r = loadProjectFile(file);
    REQUIRE_MESSAGE(r, r.errorMessage());
    CHECK(!r->warnings.empty());
    CHECK(r->project->sequences[0]->clipCount() == p->sequences[0]->clipCount());
    // Overlapping clips are removed with a warning rather than refusing to load.
    Json j = projectToJson(*p);
    auto& clips = j["project"]["sequences"][0]["tracks"][0]["clips"];
    Json dup = clips[0];
    dup["id"] = "00000000000000ff";
    clips.push_back(dup);
    auto r2 = projectFromJson(j);
    REQUIRE(r2);
    CHECK(!r2->warnings.empty());
    CHECK(validateProject(*r2->project).empty());
}

TEST_CASE("relative media paths relink moved projects") {
    auto dir = test::makeTempDir("moved");
    auto mediaDir = dir / "footage";
    std::filesystem::create_directories(mediaDir);
    auto mediaFile = mediaDir / pathFromUtf8("素材.mov");
    REQUIRE(writeFileAtomic(mediaFile, "fake"));
    Project p = makeProject("Moved");
    MediaItem m = media(pathToUtf8(mediaFile), 5);
    p.media.push_back(std::make_shared<MediaItem>(m));
    auto projFile = dir / "proj.avicap";
    REQUIRE(saveProjectFile(p, projFile));
    // Move the whole folder.
    auto moved = test::makeTempDir("moved-dest") / "project";
    std::filesystem::rename(dir, moved);
    auto r = loadProjectFile(moved / "proj.avicap");
    REQUIRE(r);
    CHECK(mediaOnline(*r->project->media[0]));
    CHECK(!r->warnings.empty());
}

TEST_CASE("journal diff replays edits exactly") {
    Document doc(richProject());
    const SequenceId sid = doc.project().activeSequence;
    ProjectPtr base = doc.current();
    std::vector<Json> diffs;
    doc.addListener([&](const ChangeEvent& ev) { diffs.push_back(diffProjects(*ev.before, *ev.after)); });
    const ClipId first = doc.project().active()->tracks[0]->clips[0]->id;
    REQUIRE(doc.editSequence("split", sid, [&](SequenceEditor& e) { return splitClip(e, first, S(3)).status(); }));
    REQUIRE(doc.editSequence("move", sid, [&](SequenceEditor& e) {
        return moveClips(e, expandSelection(e.seq(), {first}), S(20), 1, 1);
    }));
    REQUIRE(doc.editSequence("track", sid, [&](SequenceEditor& e) { return addTrack(e, TrackKind::Subtitle).status(); }));
    REQUIRE(doc.edit("rename", [&](ProjectEditor& pe) {
        pe.root().name = "Renamed";
        return Status::ok();
    }));
    REQUIRE(doc.editSequence("delete", sid, [&](SequenceEditor& e) {
        return rippleDelete(e, {e.seq().tracks[0]->clips[0]->id});
    }));
    doc.undo();
    doc.redo();
    ProjectPtr cur = base;
    for (auto& d : diffs) {
        CHECK(!d.empty());
        auto next = applyProjectDiff(cur, d);
        REQUIRE_MESSAGE(next, next.errorMessage());
        cur = *next;
    }
    Json a = projectToJson(*cur), b = projectToJson(doc.project());
    a.erase("savedUtc");
    b.erase("savedUtc");
    CHECK(a == b);
    // Unchanged snapshot -> empty diff.
    CHECK(diffProjects(doc.project(), doc.project()).empty());
}

TEST_CASE("autosave journal recovers after simulated crash") {
    auto dir = test::makeTempDir("autosave");
    AutosaveManager::Config cfg;
    cfg.directory = dir;
    cfg.snapshotIntervalSec = 3600;
    std::string expected;
    {
        Document doc(richProject());
        const SequenceId sid = doc.project().activeSequence;
        AutosaveManager as(cfg);
        as.attach(doc, "C:/projects/rich.avicap");
        for (int i = 1; i <= 25; ++i) {
            REQUIRE(doc.editSequence("split", sid, [&](SequenceEditor& e) {
                return splitAtTime(e, S(0.1 * i + 0.013)).status();
            }));
        }
        doc.undo();
        as.flush();
        CHECK(as.journalEntries() == 26);
        expected = canonical(doc.project());
        // "Crash": destroy the manager without a clean detach (files remain).
    }
    auto candidates = AutosaveManager::findRecoverable(dir, true);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].projectPath == "C:/projects/rich.avicap");
    CHECK(candidates[0].journalEntries == 26);
    auto rec = AutosaveManager::recover(candidates[0]);
    REQUIRE_MESSAGE(rec, rec.errorMessage());
    CHECK(rec->replayedEdits == 26);
    CHECK(canonical(*rec->project) == expected);

    // A torn final line (crash mid-write) is skipped, earlier entries survive.
    {
        std::FILE* f = std::fopen(pathToUtf8(candidates[0].journalFile).c_str(), "ab");
        std::fputs("{\"rev\": 99, \"ops\": [{\"op\": \"clip\", \"seq\"", f);
        std::fclose(f);
    }
    auto rec2 = AutosaveManager::recover(candidates[0]);
    REQUIRE(rec2);
    CHECK(rec2->replayedEdits == 26);
    CHECK(!rec2->warnings.empty());

    AutosaveManager::discard(candidates[0]);
    CHECK(AutosaveManager::findRecoverable(dir, true).empty());
}

TEST_CASE("autosave snapshot rotation and clean detach") {
    auto dir = test::makeTempDir("autosave2");
    AutosaveManager::Config cfg;
    cfg.directory = dir;
    cfg.snapshotIntervalSec = 3600;
    cfg.maxJournalBytes = 2000;  // force snapshots
    Document doc(richProject());
    const SequenceId sid = doc.project().activeSequence;
    AutosaveManager as(cfg);
    as.attach(doc, "");
    for (int i = 1; i <= 40; ++i)
        REQUIRE(doc.editSequence("split", sid, [&](SequenceEditor& e) { return splitAtTime(e, S(0.1 * i + 0.013)).status(); }));
    as.flush();
    auto c = AutosaveManager::findRecoverable(dir, true);
    REQUIRE(c.size() == 1);
    auto rec = AutosaveManager::recover(c[0]);
    REQUIRE(rec);
    CHECK(rec->project->sequences[0]->clipCount() == doc.project().sequences[0]->clipCount());
    as.detach(true);
    CHECK(AutosaveManager::findRecoverable(dir, true).empty());
}

TEST_CASE("folder search relinks missing media") {
    auto dir = test::makeTempDir("relink");
    std::filesystem::create_directories(dir / "a" / "b");
    REQUIRE(writeFileAtomic(dir / "a" / "b" / "clip1.mp4", "12345"));
    REQUIRE(writeFileAtomic(dir / "a" / "clip2.mp4", "123"));
    Project p = makeProject("R");
    MediaItem m1 = media("/nonexistent/old/clip1.mp4", 1);
    m1.fileSize = 5;
    MediaItem m2 = media("/nonexistent/old/clip2.mp4", 1);
    m2.fileSize = 999;  // size mismatch: must not relink
    p.media = {std::make_shared<MediaItem>(m1), std::make_shared<MediaItem>(m2)};
    auto missing = findMissingMedia(p);
    CHECK(missing.size() == 2);
    auto found = searchFolderForMedia(missing, dir);
    CHECK(found.size() == 1);
    CHECK(found.count(m1.id));
    Document doc(std::make_shared<Project>(p));
    REQUIRE(doc.edit("relink", [&](ProjectEditor& pe) { return relinkMedia(pe, m1.id, found[m1.id]); }));
    CHECK(mediaOnline(*doc.project().findMedia(m1.id)));
    CHECK(findMissingMedia(doc.project()).size() == 1);
}
