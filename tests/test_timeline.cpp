#include <doctest.h>

#include "timeline/document.h"
#include "timeline/edit_ops.h"
#include "timeline/evaluate.h"
#include "timeline/snap.h"

using namespace avc;
using namespace avc::edit;

namespace {

const Rational kFps{30, 1};
Time F(int64_t frames) { return Time::fromFrames(frames, kFps); }
Time S(double s) { return Time::fromSeconds(s); }

MediaItem fakeMedia(const std::string& name, double seconds, bool video = true, bool audio = true,
                    MediaKind kind = MediaKind::Video) {
    MediaItem m;
    m.id = newId();
    m.name = name;
    m.path = "C:/media/" + name;
    m.info.kind = kind;
    m.info.duration = S(seconds);
    if (video) {
        VideoStreamInfo v;
        v.index = 0;
        v.width = 1920;
        v.height = 1080;
        v.frameRate = kFps;
        m.info.video.push_back(v);
    }
    if (audio) {
        AudioStreamInfo a;
        a.index = 1;
        a.sampleRate = 48000;
        a.channels = 2;
        m.info.audio.push_back(a);
    }
    return m;
}

struct Fixture {
    Document doc{std::make_shared<Project>(makeProject("Test"))};
    SequenceId seq() const { return doc.project().activeSequence; }
    const Sequence& s() const { return *doc.project().active(); }
    MediaId addMedia(MediaItem m) {
        const MediaId id = m.id;
        REQUIRE(doc.edit("Import", [&](ProjectEditor& pe) {
            pe.addMedia(m);
            return Status::ok();
        }));
        return id;
    }
    std::vector<ClipId> place(MediaId media, Time at, int vTrack = 0, int aTrack = 3, PlaceMode mode = PlaceMode::Overwrite) {
        std::vector<ClipId> ids;
        Status st = doc.editSequence("Place", seq(), [&](SequenceEditor& e) -> Status {
            auto r = addMediaClip(e, *doc.project().findMedia(media), at, vTrack, aTrack, mode);
            if (!r) return r.status();
            ids = *r;
            return Status::ok();
        });
        REQUIRE_MESSAGE(st, st.message());
        return ids;
    }
    const Clip* clip(ClipId id) const { return s().clip(id); }
    const Track& track(int i) const { return *s().tracks[static_cast<size_t>(i)]; }
    Status apply(const std::function<Status(SequenceEditor&)>& fn) { return doc.editSequence("op", seq(), fn); }
};

}  // namespace

TEST_CASE("default project layout") {
    Project p = makeProject("P");
    REQUIRE(p.sequences.size() == 1);
    const Sequence& s = *p.sequences[0];
    CHECK(s.tracks.size() == 6);
    CHECK(s.visualTrackIndices() == std::vector<int>{0, 1, 2});
    CHECK(s.audioTrackIndices() == std::vector<int>{3, 4, 5});
    CHECK(validateProject(p).empty());
}

TEST_CASE("place linked A/V clip") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto ids = f.place(m, S(0));
    REQUIRE(ids.size() == 2);
    const Clip* v = f.clip(ids[0]);
    const Clip* a = f.clip(ids[1]);
    CHECK(v->duration == S(10));
    CHECK(v->linkGroup != kInvalidId);
    CHECK(v->linkGroup == a->linkGroup);
    CHECK(f.track(0).clips.size() == 1);
    CHECK(f.track(3).clips.size() == 1);
    CHECK(f.s().duration() == S(10));
}

TEST_CASE("split clip and linked partner") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto ids = f.place(m, S(0));
    std::vector<ClipId> right;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto r = splitClip(e, ids[0], S(4));
        if (!r) return r.status();
        right = *r;
        return Status::ok();
    }));
    REQUIRE(right.size() == 2);
    CHECK(f.track(0).clips.size() == 2);
    CHECK(f.track(3).clips.size() == 2);
    const Clip* l = f.clip(ids[0]);
    const Clip* r = f.s().clip(right[0]);
    CHECK(l->duration == S(4));
    CHECK(r->start == S(4));
    CHECK(r->sourceIn == S(4));
    CHECK(r->duration == S(6));
    // Right halves are linked to each other, not to the left halves.
    CHECK(f.s().clip(right[0])->linkGroup == f.s().clip(right[1])->linkGroup);
    CHECK(f.s().clip(right[0])->linkGroup != l->linkGroup);
    // Undo restores the single clip.
    CHECK(f.doc.undo());
    CHECK(f.track(0).clips.size() == 1);
    CHECK(f.doc.redo());
    CHECK(f.track(0).clips.size() == 2);
}

TEST_CASE("split outside clip fails without changes") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto ids = f.place(m, S(0));
    const auto rev = f.doc.revision();
    CHECK_FALSE(f.apply([&](SequenceEditor& e) { return splitClip(e, ids[0], S(11)).status(); }));
    CHECK(f.doc.revision() == rev);
}

TEST_CASE("overwrite placement trims and splits existing clips") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("long.mp4", 20));
    auto small = f.addMedia(fakeMedia("small.mp4", 2, true, false));
    f.place(m, S(0), 0, -1);
    f.place(small, S(5), 0, -1);
    const Track& t = f.track(0);
    REQUIRE(t.clips.size() == 3);
    CHECK(t.clips[0]->end() == S(5));
    CHECK(t.clips[1]->start == S(5));
    CHECK(t.clips[1]->duration == S(2));
    CHECK(t.clips[2]->start == S(7));
    CHECK(t.clips[2]->sourceIn == S(7));
    CHECK(t.clips[2]->end() == S(20));
}

TEST_CASE("insert placement ripples sync-locked tracks") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("long.mp4", 20));
    auto small = f.addMedia(fakeMedia("small.mp4", 2, true, false));
    f.place(m, S(0));
    f.place(small, S(5), 0, -1, PlaceMode::Insert);
    const Track& v = f.track(0);
    REQUIRE(v.clips.size() == 3);
    CHECK(v.clips[2]->start == S(7));
    CHECK(v.clips[2]->end() == S(22));
    // Audio (sync-locked) got split and shifted too, preserving sync.
    const Track& a = f.track(3);
    REQUIRE(a.clips.size() == 2);
    CHECK(a.clips[0]->end() == S(5));
    CHECK(a.clips[1]->start == S(7));
    CHECK(a.clips[1]->sourceIn == S(5));
    CHECK(f.s().duration() == S(22));
}

TEST_CASE("trim respects source limits and neighbours") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto first = f.place(m, S(0));
    auto second = f.place(m, S(10));
    // Tail extend of first is blocked by the second clip (non-ripple) and media end.
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, first[0], Edge::Tail, S(12), false); }));
    CHECK(f.clip(first[0])->end() == S(10));
    // Trim in the tail by 2 seconds (with linked audio).
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, first[0], Edge::Tail, S(8), false); }));
    CHECK(f.clip(first[0])->end() == S(8));
    CHECK(f.clip(first[1])->end() == S(8));
    CHECK(f.clip(second[0])->start == S(10));  // gap remains
    // Head trim of the second clip reveals nothing before sourceIn 0.
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, second[0], Edge::Head, S(9), false); }));
    CHECK(f.clip(second[0])->start == S(10));
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, second[0], Edge::Head, S(11), false); }));
    CHECK(f.clip(second[0])->start == S(11));
    CHECK(f.clip(second[0])->sourceIn == S(1));
    // Now it can extend back by 1s, and further only up to the neighbour end (8s) / source (10s).
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, second[0], Edge::Head, S(5), false); }));
    CHECK(f.clip(second[0])->start == S(10));
    CHECK(f.clip(second[0])->sourceIn == S(0));
}

TEST_CASE("ripple trim tail shifts later clips on all sync tracks") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto a = f.place(m, S(0));
    auto b = f.place(m, S(10));
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, a[0], Edge::Tail, S(6), true); }));
    CHECK(f.clip(a[0])->end() == S(6));
    CHECK(f.clip(b[0])->start == S(6));
    CHECK(f.clip(b[1])->start == S(6));  // audio followed
    CHECK(validateSequence(f.s()).empty());
    // Ripple extend back.
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, a[0], Edge::Tail, S(9), true); }));
    CHECK(f.clip(b[0])->start == S(9));
}

TEST_CASE("ripple trim head keeps clip start and pulls later clips") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto a = f.place(m, S(0));
    auto b = f.place(m, S(10));
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, a[0], Edge::Head, S(3), true); }));
    CHECK(f.clip(a[0])->start == S(0));
    CHECK(f.clip(a[0])->sourceIn == S(3));
    CHECK(f.clip(a[0])->duration == S(7));
    CHECK(f.clip(b[0])->start == S(7));
}

TEST_CASE("roll edit moves the cut between adjacent clips") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    auto a = f.place(m, S(0));
    // Second clip uses the middle of the media so it has head room.
    REQUIRE(f.apply([&](SequenceEditor& e) {
        return addMediaClip(e, *f.doc.project().findMedia(m), S(5), 0, 3, PlaceMode::Overwrite, TimeRange{S(3), S(5)})
            .status();
    }));
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, a[0], Edge::Tail, S(5), false); }));
    const ClipId right = f.track(0).clips[1]->id;
    REQUIRE(f.apply([&](SequenceEditor& e) { return rollEdit(e, a[0], right, S(3)); }));
    CHECK(f.clip(a[0])->end() == S(3));
    CHECK(f.clip(right)->start == S(3));
    CHECK(f.clip(right)->sourceIn == S(1));
    // Linked audio cut rolled too.
    CHECK(f.track(3).clips[0]->end() == S(3));
    CHECK(f.track(3).clips[1]->start == S(3));
    // Rolling right is limited by the left clip's source (10 s) and right clip length.
    REQUIRE(f.apply([&](SequenceEditor& e) { return rollEdit(e, a[0], right, S(20)); }));
    CHECK(f.clip(a[0])->end() == S(10) - F(1));
}

TEST_CASE("slip and slide") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 30, true, false));
    Time t0 = S(0);
    std::vector<ClipId> ids;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(f.apply([&](SequenceEditor& e) {
            auto r = addMediaClip(e, *f.doc.project().findMedia(m), t0, 0, -1, PlaceMode::Overwrite,
                                  TimeRange{S(10), S(5)});
            if (r) ids.push_back((*r)[0]);
            return r.status();
        }));
        t0 += S(5);
    }
    REQUIRE(f.apply([&](SequenceEditor& e) { return slipClip(e, ids[1], S(-4)); }));
    CHECK(f.clip(ids[1])->sourceIn == S(6));
    CHECK(f.clip(ids[1])->start == S(5));
    REQUIRE(f.apply([&](SequenceEditor& e) { return slipClip(e, ids[1], S(-100)); }));
    CHECK(f.clip(ids[1])->sourceIn == S(0));
    REQUIRE(f.apply([&](SequenceEditor& e) { return slideClip(e, ids[1], S(2)); }));
    CHECK(f.clip(ids[1])->start == S(7));
    CHECK(f.clip(ids[0])->end() == S(7));
    CHECK(f.clip(ids[2])->start == S(12));
    CHECK(f.clip(ids[2])->sourceIn == S(12));
    CHECK(validateSequence(f.s()).empty());
}

TEST_CASE("ripple delete closes gaps and keeps sync") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 5));
    auto a = f.place(m, S(0));
    auto b = f.place(m, S(5));
    auto c = f.place(m, S(10));
    REQUIRE(f.apply([&](SequenceEditor& e) { return rippleDelete(e, expandSelection(f.s(), {b[0]})); }));
    CHECK(f.track(0).clips.size() == 2);
    CHECK(f.clip(c[0])->start == S(5));
    CHECK(f.clip(c[1])->start == S(5));
    CHECK(f.s().duration() == S(10));
}

TEST_CASE("lift and extract ranges") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10));
    f.place(m, S(0));
    SUBCASE("lift") {
        REQUIRE(f.apply([&](SequenceEditor& e) { return lift(e, TimeRange{S(2), S(3)}, {0, 3}); }));
        CHECK(f.track(0).clips.size() == 2);
        CHECK(f.track(0).clips[1]->start == S(5));
        CHECK(f.s().duration() == S(10));
    }
    SUBCASE("extract") {
        REQUIRE(f.apply([&](SequenceEditor& e) { return extract(e, TimeRange{S(2), S(3)}, {0, 3}); }));
        CHECK(f.track(0).clips.size() == 2);
        CHECK(f.track(0).clips[1]->start == S(2));
        CHECK(f.track(0).clips[1]->sourceIn == S(5));
        CHECK(f.s().duration() == S(7));
    }
}

TEST_CASE("move clips across tracks and in time") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 5));
    auto a = f.place(m, S(0));
    auto sel = expandSelection(f.s(), {a[0]});
    REQUIRE(f.apply([&](SequenceEditor& e) { return moveClips(e, sel, S(3), 1, 1); }));
    CHECK(f.track(0).clips.empty());
    CHECK(f.track(1).clips.size() == 1);
    CHECK(f.track(4).clips.size() == 1);
    CHECK(f.clip(a[0])->start == S(3));
    // Out of range destination fails atomically.
    CHECK_FALSE(f.apply([&](SequenceEditor& e) { return moveClips(e, sel, S(0), 5, 0); }));
    CHECK(f.clip(a[0])->start == S(3));
    // Moving before zero clamps.
    REQUIRE(f.apply([&](SequenceEditor& e) { return moveClips(e, sel, S(-10), 0, 0); }));
    CHECK(f.clip(a[0])->start == S(0));
}

TEST_CASE("locked tracks are protected") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 5));
    auto a = f.place(m, S(0));
    REQUIRE(f.apply([&](SequenceEditor& e) {
        e.mutableTrack(0).locked = true;
        return Status::ok();
    }));
    CHECK_FALSE(f.apply([&](SequenceEditor& e) { return deleteClips(e, {a[0]}); }));
    CHECK(f.track(0).clips.size() == 1);
}

TEST_CASE("duplicate, copy and paste preserve links with new ids") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 5));
    auto a = f.place(m, S(0));
    std::vector<ClipId> dup;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto r = duplicateClips(e, expandSelection(f.s(), {a[0]}));
        if (r) dup = *r;
        return r.status();
    }));
    REQUIRE(dup.size() == 2);
    CHECK(f.s().clip(dup[0])->start == S(5));
    CHECK(f.s().clip(dup[0])->linkGroup == f.s().clip(dup[1])->linkGroup);
    CHECK(f.s().clip(dup[0])->linkGroup != f.clip(a[0])->linkGroup);

    Clipboard cb = copyClips(f.s(), expandSelection(f.s(), {a[0]}));
    REQUIRE(cb.items.size() == 2);
    std::vector<ClipId> pasted;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto r = pasteClips(e, cb, S(20), 2, 5, PlaceMode::Overwrite);
        if (r) pasted = *r;
        return r.status();
    }));
    CHECK(f.track(2).clips.size() == 1);
    CHECK(f.track(5).clips.size() == 1);
    CHECK(f.track(2).clips[0]->start == S(20));
    // Pasting to a track beyond the last one creates tracks.
    REQUIRE(f.apply([&](SequenceEditor& e) { return pasteClips(e, cb, S(0), 2, 5, PlaceMode::Overwrite).status(); }));
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto fam = e.seq().visualTrackIndices();
        return pasteClips(e, cb, S(40), fam.back(), e.seq().audioTrackIndices().back(), PlaceMode::Overwrite).status();
    }));
    CHECK(validateSequence(f.s()).empty());
}

TEST_CASE("speed change adjusts duration and ripples") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10, true, false));
    auto a = f.place(m, S(0), 0, -1);
    auto b = f.place(m, S(10), 0, -1);
    REQUIRE(f.apply([&](SequenceEditor& e) { return setClipSpeed(e, a[0], Rational{2, 1}, true); }));
    CHECK(f.clip(a[0])->duration == S(5));
    CHECK(f.clip(b[0])->start == S(5));
    CHECK(f.clip(a[0])->sourceTimeAt(S(2)) == S(4));
    REQUIRE(f.apply([&](SequenceEditor& e) { return setClipSpeed(e, a[0], Rational{1, 2}, true); }));
    CHECK(f.clip(a[0])->duration == S(20));
    CHECK(f.clip(b[0])->start == S(20));
}

TEST_CASE("reverse and freeze frame source mapping") {
    Clip c;
    c.start = S(10);
    c.duration = S(4);
    c.sourceIn = S(2);
    c.speed = {1, 1};
    CHECK(c.sourceTimeAt(S(10)) == S(2));
    c.reverse = true;
    CHECK(c.sourceTimeAt(S(10)) == S(6) - Time{1});
    CHECK(c.sourceTimeAt(S(14) - Time{1}) >= S(2));
    c.reverse = false;
    c.freezeFrame = true;
    CHECK(c.sourceTimeAt(S(13)) == S(2));
}

TEST_CASE("freeze frame insertion") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10, true, false));
    auto a = f.place(m, S(0), 0, -1);
    ClipId freeze = 0;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto r = insertFreezeFrame(e, a[0], S(4), S(2));
        if (r) freeze = *r;
        return r.status();
    }));
    const Track& t = f.track(0);
    REQUIRE(t.clips.size() == 3);
    CHECK(t.clips[1]->id == freeze);
    CHECK(t.clips[1]->freezeFrame);
    CHECK(t.clips[1]->sourceIn == S(4));
    CHECK(t.clips[2]->start == S(6));
    CHECK(t.clips[2]->sourceIn == S(4));
}

TEST_CASE("undo merge key coalesces slider drags") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10, true, false));
    auto a = f.place(m, S(0), 0, -1);
    const size_t before = f.doc.undoHistory().size();
    for (int i = 0; i <= 10; ++i) {
        EditOptions opt;
        opt.mergeKey = "opacity";
        REQUIRE(f.doc.editSequence("Opacity", f.seq(), [&](SequenceEditor& e) {
            e.mutableClip(a[0]).transform.setStatic("opacity", pv(100.0f - static_cast<float>(i) * 5));
            return Status::ok();
        }, opt));
    }
    CHECK(f.doc.undoHistory().size() == before + 1);
    CHECK(f.clip(a[0])->transform.evaluate1("opacity", Time{0}) == doctest::Approx(50));
    f.doc.undo();
    CHECK(f.clip(a[0])->transform.evaluate1("opacity", Time{0}) == doctest::Approx(100));
}

TEST_CASE("undo groups and abort") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10, true, false));
    auto a = f.place(m, S(0), 0, -1);
    const size_t before = f.doc.undoHistory().size();
    f.doc.beginGroup("AI: remove silence");
    REQUIRE(f.apply([&](SequenceEditor& e) { return splitClip(e, a[0], S(2)).status(); }));
    REQUIRE(f.apply([&](SequenceEditor& e) { return splitAtTime(e, S(5)).status(); }));
    f.doc.endGroup();
    CHECK(f.doc.undoHistory().size() == before + 1);
    CHECK(f.track(0).clips.size() == 3);
    f.doc.undo();
    CHECK(f.track(0).clips.size() == 1);

    f.doc.beginGroup("will abort");
    REQUIRE(f.apply([&](SequenceEditor& e) { return splitClip(e, a[0], S(2)).status(); }));
    f.doc.abortGroup();
    CHECK(f.track(0).clips.size() == 1);
    CHECK(f.doc.canRedo());  // redo stack untouched by aborted group
}

TEST_CASE("undo limit and memory sharing") {
    Fixture f;
    f.doc.setUndoLimit(1000);
    auto m = f.addMedia(fakeMedia("a.mp4", 1000, true, false));
    auto a = f.place(m, S(0), 0, -1);
    (void)a;
    for (int i = 1; i < 1200; ++i) {
        REQUIRE(f.apply([&](SequenceEditor& e) { return splitAtTime(e, F(i * 10)).status(); }));
    }
    CHECK(f.doc.undoHistory().size() == 1000);
    CHECK(f.track(0).clips.size() == 1200);
    // Structural sharing: an untouched track is the same object in consecutive snapshots.
    const auto& hist = f.doc.undoHistory();
    CHECK(hist.back().before->active()->tracks[1] == hist.back().after->active()->tracks[1]);
    for (int i = 0; i < 1000; ++i) REQUIRE(f.doc.undo());
    CHECK_FALSE(f.doc.canUndo());
}

TEST_CASE("track management keeps visual tracks above audio") {
    Fixture f;
    TrackId textTrack = 0;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto r = addTrack(e, TrackKind::Text);
        if (r) textTrack = *r;
        return r.status();
    }));
    CHECK(f.s().trackIndex(textTrack) == 3);
    REQUIRE(f.apply([&](SequenceEditor& e) { return addTrack(e, TrackKind::Audio).status(); }));
    CHECK(f.s().tracks.back()->kind == TrackKind::Audio);
    REQUIRE(f.apply([&](SequenceEditor& e) { return moveTrack(e, textTrack, 0); }));
    CHECK(f.s().trackIndex(textTrack) == 0);
    CHECK(validateSequence(f.s()).empty());
    REQUIRE(f.apply([&](SequenceEditor& e) { return removeTrack(e, textTrack); }));
    CHECK(f.s().findTrack(textTrack) == nullptr);
}

TEST_CASE("text clips only on compatible tracks") {
    Fixture f;
    Clip t = makeTextClip("Hello", S(0), S(3));
    CHECK(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, t}}, PlaceMode::Overwrite); }));
    Clip t2 = makeTextClip("World", S(0), S(3));
    CHECK_FALSE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{3, t2}}, PlaceMode::Overwrite); }));
}

TEST_CASE("markers stay sorted") {
    Fixture f;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        addMarker(e, Marker{0, S(5), {}, "b"});
        addMarker(e, Marker{0, S(1), {}, "a"});
        Marker beat;
        beat.time = S(3);
        beat.kind = MarkerKind::Beat;
        addMarker(e, beat);
        return Status::ok();
    }));
    CHECK(f.s().markers.size() == 3);
    CHECK(f.s().markers[0].name == "a");
    CHECK(f.s().markers[1].kind == MarkerKind::Beat);
    REQUIRE(f.apply([&](SequenceEditor& e) {
        removeMarkersOfKind(e, MarkerKind::Beat);
        return Status::ok();
    }));
    CHECK(f.s().markers.size() == 2);
}

TEST_CASE("snapping to clips, playhead and markers") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 5));
    auto a = f.place(m, S(0));
    auto b = f.place(m, S(10));
    REQUIRE(f.apply([&](SequenceEditor& e) {
        Marker mk;
        mk.time = S(7);
        addMarker(e, mk);
        return Status::ok();
    }));
    auto pts = collectSnapPoints(f.s(), S(8), {b[0], b[1]});
    auto r = snapTime(pts, S(5.05), S(0.1));
    CHECK(r.snapped);
    CHECK(r.time == S(5));
    CHECK(r.point.kind == SnapKind::ClipEnd);
    r = snapTime(pts, S(7.02), S(0.1));
    CHECK(r.point.kind == SnapKind::Marker);
    r = snapTime(pts, S(8.03), S(0.1));
    CHECK(r.point.kind == SnapKind::Playhead);
    r = snapTime(pts, S(6.0), S(0.1));
    CHECK_FALSE(r.snapped);
    // Moving clip b (10..15) left by 4.97 s: its start snaps to a's end at 5.
    auto mr = snapMove(pts, f.clip(b[0])->range(), S(-4.97), S(0.1));
    CHECK(mr.snapped);
    CHECK(f.clip(b[0])->start + mr.delta == S(5));
}

TEST_CASE("keyframe interpolation") {
    AnimatedParam p(pv(0));
    p.addKey({S(0), pv(0), Interp::Linear});
    p.addKey({S(2), pv(100), Interp::Hold});
    p.addKey({S(4), pv(50), Interp::Linear});
    CHECK(p.evaluate(S(1))[0] == doctest::Approx(50));
    CHECK(p.evaluate(S(3))[0] == doctest::Approx(100));  // hold
    CHECK(p.evaluate(S(4))[0] == doctest::Approx(50));
    CHECK(p.evaluate(S(10))[0] == doctest::Approx(50));
    CHECK(p.evaluate(S(-1))[0] == doctest::Approx(0));
    AnimatedParam e(pv(0));
    e.addKey({S(0), pv(0), Interp::EaseInOut});
    e.addKey({S(1), pv(1), Interp::Linear});
    CHECK(e.evaluate(S(0.5))[0] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(e.evaluate(S(0.1))[0] < 0.1f);
    CHECK(cubicBezierEase(0.5f, 0.25f, 0.1f, 0.25f, 1.0f) == doctest::Approx(0.8024).epsilon(0.01));
    // setValueAt on an animated param adds a key.
    e.setValueAt(S(0.5), pv(0.9f));
    CHECK(e.keys().size() == 3);
}

TEST_CASE("head trim shifts keyframes to stay with content") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10, true, false));
    auto a = f.place(m, S(0), 0, -1);
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto& p = e.mutableClip(a[0]).transform.getOrAdd("opacity");
        p.addKey({S(5), pv(0)});
        p.addKey({S(6), pv(100)});
        return Status::ok();
    }));
    REQUIRE(f.apply([&](SequenceEditor& e) { return trimClip(e, a[0], Edge::Head, S(2), false); }));
    const Clip* c = f.clip(a[0]);
    // The key that was at timeline 5 s is still at timeline 5 s.
    const auto& keys = c->transform.find("opacity")->keys();
    CHECK(c->start + keys[0].time == S(5));
}

TEST_CASE("frame plan evaluates layers and transitions") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 10, true, false));
    auto a = f.place(m, S(0), 0, -1);
    Clip title = makeTextClip("Title", S(1), S(2));
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{1, title}}, PlaceMode::Overwrite); }));
    auto plan = planFrame(f.doc.current(), f.s(), S(1.5));
    REQUIRE(plan.layers.size() == 2);
    CHECK(plan.layers[0].a.clip->id == a[0]);
    CHECK(plan.layers[1].a.clip->kind == ClipKind::Text);
    CHECK(plan.layers[0].a.sourceTime == S(1.5));

    // Cross dissolve between two clips (centered 1 s transition at 5 s).
    REQUIRE(f.apply([&](SequenceEditor& e) { return splitClip(e, a[0], S(5)).status(); }));
    const ClipId right = f.track(0).clips[1]->id;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        e.mutableClip(right).transitionIn = TransitionSpec{"cross-dissolve", S(1), {}};
        return Status::ok();
    }));
    plan = planFrame(f.doc.current(), f.s(), S(5));
    REQUIRE(!plan.layers.empty());
    CHECK(plan.layers[0].isTransition());
    CHECK(plan.layers[0].progress == doctest::Approx(0.5));
    CHECK(plan.layers[0].a.clip->id == a[0]);
    CHECK(plan.layers[0].a.sourceTime == S(5));  // handle beyond the cut
    CHECK(plan.layers[0].b.clip->id == right);
    plan = planFrame(f.doc.current(), f.s(), S(4.4));
    CHECK_FALSE(plan.layers[0].isTransition());
    // Hidden track removes layer.
    REQUIRE(f.apply([&](SequenceEditor& e) {
        e.mutableTrack(1).hidden = true;
        return Status::ok();
    }));
    plan = planFrame(f.doc.current(), f.s(), S(1.5));
    CHECK(plan.layers.size() == 1);
}

TEST_CASE("audio solo / mute and crossfade gain") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.wav", 10, false, true, MediaKind::Audio));
    f.place(m, S(0), -1, 3);
    f.place(m, S(0), -1, 4);
    CHECK(activeAudioClips(f.s(), TimeRange{S(1), S(1)}).size() == 2);
    REQUIRE(f.apply([&](SequenceEditor& e) {
        e.mutableTrack(4).solo = true;
        return Status::ok();
    }));
    auto act = activeAudioClips(f.s(), TimeRange{S(1), S(1)});
    REQUIRE(act.size() == 1);
    CHECK(act[0].trackIndex == 4);
    const Track& t = f.track(4);
    CHECK(audioTransitionGain(t, *t.clips[0], S(1)) == doctest::Approx(1.0f));
    CHECK(audioTransitionGain(t, *t.clips[0], S(11)) == doctest::Approx(0.0f));
}

TEST_CASE("compound clip nests selection") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 4));
    auto a = f.place(m, S(2));
    auto b = f.place(m, S(6));
    SequenceId nested = 0;
    REQUIRE(f.doc.edit("Nest", [&](ProjectEditor& pe) {
        auto r = createCompoundClip(pe, f.seq(), {a[0], b[0]}, "Nest");
        if (r) nested = *r;
        return r.status();
    }));
    const Sequence* ns = f.doc.project().findSequence(nested);
    REQUIRE(ns);
    CHECK(ns->clipCount() == 4);
    CHECK(ns->duration() == S(8));
    const Track& v = f.track(0);
    REQUIRE(v.clips.size() == 1);
    CHECK(v.clips[0]->kind == ClipKind::Compound);
    CHECK(v.clips[0]->start == S(2));
    CHECK(v.clips[0]->duration == S(8));
    CHECK(f.track(3).clips[0]->kind == ClipKind::Compound);
    CHECK(validateProject(f.doc.project()).empty());
}

TEST_CASE("stress: 1000 clips over 20 tracks [stress]") {
    Fixture f;
    auto m = f.addMedia(fakeMedia("a.mp4", 3 * 3600));
    REQUIRE(f.apply([&](SequenceEditor& e) {
        for (int i = 0; i < 17; ++i)
            if (auto r = addTrack(e, TrackKind::Video); !r) return r.status();
        return Status::ok();
    }));
    REQUIRE(f.apply([&](SequenceEditor& e) {
        const MediaItem& mi = *f.doc.project().findMedia(m);
        for (int t = 0; t < 20; ++t) {
            for (int i = 0; i < 50; ++i) {
                Clip c = makeMediaClip(mi, TrackKind::Video, S(i * 10.0), S(i * 5.0), S(8));
                if (!e.insertClip(t, std::move(c))) return Status::error("insert failed");
            }
        }
        return Status::ok();
    }));
    CHECK(f.s().clipCount() == 1000);
    // Many edits with undo stay fast and valid.
    for (int i = 0; i < 200; ++i) {
        REQUIRE(f.apply([&](SequenceEditor& e) { return splitAtTime(e, S(i * 2.5 + 0.5)).status(); }));
    }
    CHECK(validateSequence(f.s()).empty());
    for (int i = 0; i < 100; ++i) {
        auto plan = planFrame(f.doc.current(), f.s(), S(i * 4.2));
        CHECK(plan.layers.size() <= 20);
    }
}
