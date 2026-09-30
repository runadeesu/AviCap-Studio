// Golden editing path, end to end:
//   import -> timeline -> split -> text -> audio -> save -> reload -> export
//   -> verify / probe output -> decode exported frames and audio.

#include <doctest.h>

#include <cmath>
#include <thread>

#include "core/strings.h"
#include "decode/audio_reader.h"
#include "decode/video_decoder.h"
#include "encode/encoder.h"
#include "export/exporter.h"
#include "media/probe.h"
#include "project/serialize.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;
using namespace avc::edit;

namespace {
Time S(double s) { return Time::fromSeconds(s); }

double meanRed(const VideoFrame& f, int x0, int y0, int x1, int y1) {
    std::vector<uint8_t> rgba(static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 4);
    convertToRgba8(f, rgba.data(), f.width * 4);
    double s = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            s += rgba[(static_cast<size_t>(y) * static_cast<size_t>(f.width) + static_cast<size_t>(x)) * 4];
            ++n;
        }
    return s / std::max(1, n);
}
}  // namespace

TEST_CASE("golden path: import, edit, save, reload, export, verify") {
    const std::string video = test::testMedia("av_1080p30.mp4");
    const std::string voice = test::testMedia("speech_silence.wav");
    if (video.empty() || voice.empty()) {
        MESSAGE("test media missing");
        return;
    }
    auto dir = test::makeTempDir("golden");

    // 1) New project, import media.
    Document doc(std::make_shared<Project>(makeProject("Golden", ProjectSettings{1280, 720, {30, 1}, 48000})));
    const SequenceId sid = doc.project().activeSequence;
    auto vm = createMediaItem(video);
    auto am = createMediaItem(voice);
    REQUIRE(vm);
    REQUIRE(am);
    REQUIRE(doc.edit("Import", [&](ProjectEditor& pe) {
        pe.addMedia(*vm);
        pe.addMedia(*am);
        return Status::ok();
    }));

    // 2) Timeline: A/V clip at 0, split at 2 s, delete the tail after 3.5 s.
    std::vector<ClipId> av;
    REQUIRE(doc.editSequence("Add", sid, [&](SequenceEditor& e) {
        auto r = addMediaClip(e, *doc.project().findMedia(vm->id), S(0), 0, 3, PlaceMode::Overwrite);
        if (r) av = *r;
        return r.status();
    }));
    REQUIRE(doc.editSequence("Split", sid, [&](SequenceEditor& e) { return splitClip(e, av[0], S(2)).status(); }));
    REQUIRE(doc.editSequence("Trim", sid, [&](SequenceEditor& e) {
        const ClipId right = e.seq().tracks[0]->clips[1]->id;
        return trimClip(e, right, Edge::Tail, S(3.5), false);
    }));
    // 3) Title with red fill in the upper part of the frame.
    REQUIRE(doc.editSequence("Title", sid, [&](SequenceEditor& e) {
        Clip t = makeTextClip("GOLDEN", S(0.5), S(2.5));
        t.textStyle.fontFamily = "DejaVu Sans";
        t.textStyle.fontSize = 120;
        t.textParams.setStatic("fillColor", pv(1, 0, 0, 1));
        t.textParams.setStatic("backgroundColor", pv(1, 0, 0, 1));
        t.transform.setStatic("position", pv(0, -220));
        return placeClips(e, {{1, t}}, PlaceMode::Overwrite);
    }));
    // 4) Voice-over on A2 starting at 1 s, with a fade-in.
    REQUIRE(doc.editSequence("Voice", sid, [&](SequenceEditor& e) {
        auto r = addMediaClip(e, *doc.project().findMedia(am->id), S(1), -1, 4, PlaceMode::Overwrite);
        if (!r) return r.status();
        e.mutableClip((*r)[0]).transitionIn = TransitionSpec{"cross-dissolve", S(0.2), {}};
        return Status::ok();
    }));
    CHECK(doc.project().active()->duration() == S(10.0));  // voice-over (1 s + 9 s) is the longest

    // 5) Save and reload.
    const auto projFile = dir / "golden.avicap";
    REQUIRE(saveProjectFile(doc.project(), projFile));
    auto loaded = loadProjectFile(projFile);
    REQUIRE_MESSAGE(loaded, loaded.errorMessage());
    const Project& lp = *loaded->project;
    REQUIRE(lp.active());
    CHECK(lp.active()->clipCount() == doc.project().active()->clipCount());

    // 6) Export the first 3 seconds at 640x360 (H.264 + AAC) and verify.
    exp::ExportSettings es;
    es.outputPath = pathToUtf8(dir / "golden.mp4");
    es.container = "mp4";
    es.videoCodec = enc::VideoCodec::H264;
    es.width = 640;
    es.height = 360;
    es.bitrateKbps = 2000;
    es.range = TimeRange{S(0), S(3)};
    const enc::EncoderInfo* h264 = enc::EncoderCatalog::instance().best(enc::VideoCodec::H264, false);
    if (!h264) {
        MESSAGE("no H.264 encoder in this FFmpeg build; using MPEG-4 Part 2");
        es.videoCodec = enc::VideoCodec::MPEG4;
    }
    exp::ExportJob job(loaded->project, lp.activeSequence, es, [] { return gpu::createCpuDevice(); });
    Status st = job.run(CancelToken::create());
    REQUIRE_MESSAGE(st, st.message());
    const auto prog = job.progress();
    CHECK(prog.state == exp::ExportState::Done);
    CHECK(prog.framesDone == 90);
    const auto& rep = job.report();
    CHECK_MESSAGE(rep.ok, rep.summary());
    CHECK(rep.width == 640);
    CHECK(rep.height == 360);
    CHECK(rep.durationSec == doctest::Approx(3.0).epsilon(0.05));
    CHECK(rep.audioChannels == 2);
    CHECK(rep.sampleRate == 48000);
    // No partial file left behind.
    CHECK_FALSE(std::filesystem::exists(dir / "golden.partial.mp4"));

    // 7) Output content: title box is red in the upper area during 0.5-3 s,
    //    and the voice-over tone is audible at 1.5 s but not at 0.5 s.
    auto dec = openVideoDecoder(es.outputPath);
    REQUIRE(dec);
    auto f1 = (*dec)->frameAt(S(1.0));
    REQUIRE(f1);
    const double redTop = meanRed(**f1, 300, 40, 340, 70);
    CHECK(redTop > 150);
    auto f0 = (*dec)->frameAt(S(0.1));
    REQUIRE(f0);
    CHECK(meanRed(**f0, 300, 40, 340, 70) < redTop);  // before the title appears
    auto audio = decodeAllAudio(es.outputPath, AudioFormat{48000, 2});
    REQUIRE(audio);
    auto rmsAt = [&](double sec) {
        double acc = 0;
        const size_t a = static_cast<size_t>(sec * 48000) * 2;
        for (size_t i = a; i < a + 4800 * 2 && i < audio->size(); ++i) acc += double((*audio)[i]) * (*audio)[i];
        return std::sqrt(acc / 9600.0);
    };
    // The source video carries a 440 Hz tone (A1) throughout; A2 adds 300 Hz from 1 s.
    CHECK(rmsAt(0.5) > 0.05);  // lavfi sine default amplitude 1/8
    CHECK(rmsAt(1.5) > rmsAt(0.5) * 1.1);
}

TEST_CASE("export presets and codecs") {
    CHECK(exp::presets().size() >= 7);
    CHECK(exp::findPreset("youtube-1080p"));
    CHECK(exp::findPreset("tiktok")->settings.height == 1920);
    auto s = exp::applyPreset("master-prores", exp::ExportSettings{});
    CHECK(s.videoCodec == enc::VideoCodec::ProRes);
    CHECK(s.container == "mov");
    CHECK(exp::estimateOutputBytes(s, 1920, 1080, 30, 60) > 1000000000ull);
    MESSAGE("Encoders: ", enc::EncoderCatalog::instance().summary());
}

TEST_CASE("export ProRes master with letterboxed vertical output") {
    auto dir = test::makeTempDir("prores");
    Document doc(std::make_shared<Project>(makeProject("P", ProjectSettings{320, 180, {25, 1}, 48000})));
    Clip c = makeSolidClip(pv(0, 1, 0, 1), S(0), S(1));
    REQUIRE(doc.editSequence("p", doc.project().activeSequence, [&](SequenceEditor& e) {
        return placeClips(e, {{0, c}}, PlaceMode::Overwrite);
    }));
    if (!enc::EncoderCatalog::instance().best(enc::VideoCodec::ProRes)) return;
    exp::ExportSettings es = exp::applyPreset("master-prores", {});
    es.outputPath = pathToUtf8(dir / "master.mov");
    es.width = 180;
    es.height = 320;  // vertical: 16:9 content letterboxed
    exp::ExportJob job(doc.current(), doc.project().activeSequence, es, [] { return gpu::createCpuDevice(); });
    Status st = job.run(CancelToken::create());
    REQUIRE_MESSAGE(st, st.message());
    CHECK(job.report().videoCodec == "prores");
    auto dec = openVideoDecoder(es.outputPath);
    REQUIRE(dec);
    auto f = (*dec)->frameAt(S(0.5));
    REQUIRE(f);
    std::vector<uint8_t> rgba(static_cast<size_t>((*f)->width) * static_cast<size_t>((*f)->height) * 4);
    convertToRgba8(**f, rgba.data(), (*f)->width * 4);
    auto px = [&](int x, int y) { return rgba[(static_cast<size_t>(y) * 180 + static_cast<size_t>(x)) * 4 + 1]; };
    CHECK(px(90, 160) > 200);  // green in the middle band
    CHECK(px(90, 10) < 30);    // black letterbox above
}

TEST_CASE("export cancel removes partial output and queue processes jobs") {
    auto dir = test::makeTempDir("queue");
    Document doc(std::make_shared<Project>(makeProject("Q", ProjectSettings{160, 90, {30, 1}, 48000})));
    Clip c = makeSolidClip(pv(1, 1, 1, 1), S(0), S(2));
    REQUIRE(doc.editSequence("p", doc.project().activeSequence, [&](SequenceEditor& e) {
        return placeClips(e, {{0, c}}, PlaceMode::Overwrite);
    }));
    exp::ExportSettings es;
    es.outputPath = pathToUtf8(dir / "a.mp4");
    es.videoCodec = enc::EncoderCatalog::instance().best(enc::VideoCodec::H264) ? enc::VideoCodec::H264 : enc::VideoCodec::MPEG4;
    es.bitrateKbps = 500;
    auto cancelToken = CancelToken::create();
    cancelToken.cancel();
    exp::ExportJob cancelled(doc.current(), doc.project().activeSequence, es, [] { return gpu::createCpuDevice(); });
    CHECK_FALSE(cancelled.run(cancelToken));
    CHECK(cancelled.progress().state == exp::ExportState::Cancelled);
    CHECK_FALSE(std::filesystem::exists(dir / "a.mp4"));
    CHECK_FALSE(std::filesystem::exists(dir / "a.partial.mp4"));

    exp::ExportQueue queue;
    auto j1 = std::make_shared<exp::ExportJob>(doc.current(), doc.project().activeSequence, es, [] { return gpu::createCpuDevice(); });
    es.outputPath = pathToUtf8(dir / "b.mp4");
    auto j2 = std::make_shared<exp::ExportJob>(doc.current(), doc.project().activeSequence, es, [] { return gpu::createCpuDevice(); });
    queue.add(j1);
    queue.add(j2);
    for (int i = 0; i < 600 && queue.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(j1->progress().state == exp::ExportState::Done);
    CHECK(j2->progress().state == exp::ExportState::Done);
    CHECK(std::filesystem::exists(dir / "a.mp4"));
    CHECK(std::filesystem::exists(dir / "b.mp4"));
}
