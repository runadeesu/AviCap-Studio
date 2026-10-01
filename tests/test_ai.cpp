#include <doctest.h>

#include <cmath>

#include "ai/analysis.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"
#include "media/probe.h"

using namespace avc;

namespace {
Time S(double s) { return Time::fromSeconds(s); }

std::vector<float> toneWithGaps(int rate, std::initializer_list<std::pair<double, double>> on, double total) {
    std::vector<float> x(static_cast<size_t>(total * rate));
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        bool active = false;
        for (const auto& [a, b] : on) active |= t >= a && t < b;
        // Speech-like tone plus a faint noise floor everywhere.
        x[i] = (active ? 0.4f * static_cast<float>(std::sin(2 * M_PI * 220 * t)) : 0.0f) +
               0.002f * static_cast<float>(std::sin(2 * M_PI * 3001 * t) * std::sin(2 * M_PI * 17 * t));
    }
    return x;
}
}  // namespace

TEST_CASE("silence detection on samples with padding and minimum length") {
    const int rate = 16000;
    auto x = toneWithGaps(rate, {{0, 1}, {2.5, 4.5}, {4.7, 6}, {8, 9}}, 9.0);
    ai::SilenceOptions opt;
    opt.minSilenceSec = 0.5;
    opt.paddingSec = 0.1;
    auto r = ai::detectSilenceInSamples(x.data(), x.size(), rate, opt);
    REQUIRE(r.silences.size() == 2);  // the 0.2 s pause at 4.5 s is kept
    CHECK(r.silences[0].start.seconds() == doctest::Approx(1.1).epsilon(0.02));
    CHECK(r.silences[0].end().seconds() == doctest::Approx(2.4).epsilon(0.02));
    CHECK(r.silences[1].start.seconds() == doctest::Approx(6.1).epsilon(0.02));
    CHECK(r.silences[1].end().seconds() == doctest::Approx(7.9).epsilon(0.02));
    CHECK(r.thresholdDb > r.noiseFloorDb);

    opt.autoThreshold = false;
    opt.thresholdDb = -20.0f;  // everything below -20 dBFS counts as silence
    auto loud = ai::detectSilenceInSamples(x.data(), x.size(), rate, opt);
    CHECK(loud.silences.size() == 2);
}

TEST_CASE("silence detection on a file") {
    const std::string f = test::testMedia("speech_silence.wav");
    if (f.empty()) return;
    auto r = ai::detectSilence(f, ai::SilenceOptions{});
    REQUIRE_MESSAGE(r, r.errorMessage());
    REQUIRE(r->silences.size() == 2);
    CHECK(r->silences[0].start.seconds() == doctest::Approx(1.12).epsilon(0.03));
    CHECK(r->silences[1].end().seconds() == doctest::Approx(7.88).epsilon(0.03));
    CHECK(r->durationSec == doctest::Approx(9.0).epsilon(0.01));
}

TEST_CASE("beat detection finds 120 BPM") {
    const std::string f = test::testMedia("beats_120bpm.wav");
    if (f.empty()) return;
    auto r = ai::detectBeats(f);
    REQUIRE_MESSAGE(r, r.errorMessage());
    CHECK(r->bpm == doctest::Approx(120.0).epsilon(0.02));
    REQUIRE(r->beats.size() >= 14);
    for (size_t i = 0; i < r->beats.size(); ++i) {
        const double t = r->beats[i].seconds();
        const double nearest = std::round(t / 0.5) * 0.5;
        CHECK(std::fabs(t - nearest) < 0.03);
    }
}

TEST_CASE("scene detection finds the colour changes") {
    const std::string f = test::testMedia("scenes.mp4");
    if (f.empty()) return;
    auto r = ai::detectScenes(f, ai::SceneOptions{});
    REQUIRE_MESSAGE(r, r.errorMessage());
    REQUIRE(r->cuts.size() == 2);
    CHECK(r->cuts[0].seconds() == doctest::Approx(2.0).epsilon(0.03));
    CHECK(r->cuts[1].seconds() == doctest::Approx(4.0).epsilon(0.03));
}

TEST_CASE("highlights pick the loudest moments") {
    const int rate = 8000;
    std::vector<float> x(static_cast<size_t>(20 * rate));
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const float amp = (t >= 12 && t < 14) ? 0.8f : (t >= 4 && t < 5) ? 0.5f : 0.05f;
        x[i] = amp * static_cast<float>(std::sin(2 * M_PI * 300 * t));
    }
    auto h = ai::findHighlightsInSamples(x.data(), x.size(), rate, 2, 1.0);
    REQUIRE(h.size() == 2);
    CHECK(h[0].range.start.seconds() == doctest::Approx(4.0).epsilon(0.1));
    CHECK(h[1].range.start.seconds() >= 12.0 - 0.15);
    CHECK(h[1].range.end().seconds() <= 14.0 + 0.15);
}

TEST_CASE("source ranges map to the timeline through speed and offsets") {
    MediaItem m;
    m.id = 1;
    Clip c;
    c.kind = ClipKind::Media;
    c.start = S(10);
    c.sourceIn = S(4);
    c.duration = S(3);
    c.speed = Rational{2, 1};  // consumes 6 s of source
    auto out = ai::sourceRangesToTimeline(c, {TimeRange::fromStartEnd(S(5), S(7)), TimeRange::fromStartEnd(S(0), S(1))});
    REQUIRE(out.size() == 1);
    CHECK(out[0].start.seconds() == doctest::Approx(10.5));
    CHECK(out[0].end().seconds() == doctest::Approx(11.5));
    c.reverse = true;
    out = ai::sourceRangesToTimeline(c, {TimeRange::fromStartEnd(S(4), S(6))});
    REQUIRE(out.size() == 1);
    CHECK(out[0].start.seconds() == doctest::Approx(12.0));
    CHECK(out[0].end().seconds() == doctest::Approx(13.0));
}

TEST_CASE("ripple removal keeps all tracks in sync and moves markers") {
    const std::string f = test::testMedia("av_1080p30.mp4");
    if (f.empty()) return;
    auto media = createMediaItem(f);
    REQUIRE(media);
    Document doc(std::make_shared<Project>(makeProject("AI", ProjectSettings{1280, 720, {30, 1}, 48000})));
    REQUIRE(doc.edit("import", [&](ProjectEditor& pe) {
        pe.addMedia(*media);
        return Status::ok();
    }));
    const SequenceId sid = doc.project().activeSequence;
    REQUIRE(doc.editSequence("add", sid, [&](SequenceEditor& e) {
        auto r = edit::addMediaClip(e, *media, S(0), 0, 3, edit::PlaceMode::Overwrite);
        Marker mk;
        mk.time = S(4.0);
        mk.name = "after";
        edit::addMarker(e, mk);
        Marker in;
        in.time = S(1.5);
        edit::addMarker(e, in);
        return r.status();
    }));
    REQUIRE(doc.editSequence("cut", sid, [&](SequenceEditor& e) {
        return ai::rippleRemoveRanges(e, {TimeRange::fromStartEnd(S(1), S(2)), TimeRange::fromStartEnd(S(3), S(3.5)),
                                          TimeRange::fromStartEnd(S(1.5), S(2.2))});
    }));
    const Sequence& s = *doc.project().active();
    CHECK(s.duration().seconds() == doctest::Approx(5.0 - 1.2 - 0.5).epsilon(0.001));
    // Video (V1) and audio (A1) cut identically.
    const Track& v = *s.tracks[0];
    const Track& a = *s.tracks[3];
    REQUIRE(v.clips.size() == a.clips.size());
    for (size_t i = 0; i < v.clips.size(); ++i) {
        CHECK(v.clips[i]->start == a.clips[i]->start);
        CHECK(v.clips[i]->duration == a.clips[i]->duration);
        CHECK(v.clips[i]->sourceIn == a.clips[i]->sourceIn);
    }
    REQUIRE(s.markers.size() == 1);  // the marker inside a removed range is gone
    CHECK(s.markers[0].time.seconds() == doctest::Approx(4.0 - 1.2 - 0.5).epsilon(0.001));
    CHECK(doc.undoHistory().size() == 3);  // one undo step for the whole cut
}
