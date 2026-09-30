#include <doctest.h>

#include <cmath>
#include <vector>

#include "decode/audio_reader.h"
#include "decode/frame_cache.h"
#include "decode/video_decoder.h"
#include "media/probe.h"
#include "tests/test_support.h"

using namespace avc;

#define REQUIRE_MEDIA(var, name)                                   \
    const std::string var = test::testMedia(name);                 \
    if (var.empty()) {                                             \
        MESSAGE("test media missing: " name " (run make_test_media)"); \
        return;                                                    \
    }

namespace {

// Mean RGB of a frame region (after conversion).
std::array<double, 3> meanRgb(const VideoFrame& f) {
    std::vector<uint8_t> rgba(static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 4);
    convertToRgba8(f, rgba.data(), f.width * 4);
    std::array<double, 3> m{};
    const size_t n = static_cast<size_t>(f.width) * static_cast<size_t>(f.height);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) m[static_cast<size_t>(c)] += rgba[i * 4 + static_cast<size_t>(c)];
    for (auto& v : m) v /= static_cast<double>(n);
    return m;
}

}  // namespace

TEST_CASE("probe detects containers, streams and kinds") {
    REQUIRE_MEDIA(av, "av_1080p30.mp4");
    auto info = probeMedia(av);
    REQUIRE_MESSAGE(info, info.errorMessage());
    CHECK(info->kind == MediaKind::Video);
    REQUIRE(info->video.size() == 1);
    CHECK(info->video[0].width == 1920);
    CHECK(info->video[0].height == 1080);
    CHECK(info->video[0].frameRate == Rational{30, 1});
    CHECK(info->video[0].codec == "h264");
    REQUIRE(info->audio.size() == 1);
    CHECK(info->audio[0].sampleRate == 48000);
    CHECK(info->audio[0].channels == 2);
    CHECK(info->duration.seconds() == doctest::Approx(5.0).epsilon(0.03));

    REQUIRE_MEDIA(ntsc, "ntsc_720p.mp4");
    auto n = probeMedia(ntsc);
    REQUIRE(n);
    CHECK(n->video[0].frameRate == Rational{30000, 1001});

    REQUIRE_MEDIA(png, "still.png");
    auto p = probeMedia(png);
    REQUIRE(p);
    CHECK(p->kind == MediaKind::Image);
    CHECK(p->video[0].width == 800);

    REQUIRE_MEDIA(wav, "speech_silence.wav");
    auto w = probeMedia(wav);
    REQUIRE(w);
    CHECK(w->kind == MediaKind::Audio);
    CHECK(w->duration.seconds() == doctest::Approx(9.0).epsilon(0.01));

    for (const char* name : {"vp9.webm", "prores.mov", "mono_44k.mp3", "mpeg2.ts", "photo.jpg", "scenes.mp4"}) {
        const std::string path = test::testMedia(name);
        if (path.empty()) continue;
        auto r = probeMedia(path);
        CHECK_MESSAGE(r, name, ": ", r.errorMessage());
    }
    CHECK_FALSE(probeMedia("/nonexistent/file.mp4"));
}

TEST_CASE("unicode paths import") {
    REQUIRE_MEDIA(jp, "日本語クリップ.mp4");
    auto m = createMediaItem(jp);
    REQUIRE_MESSAGE(m, m.errorMessage());
    CHECK(m->name == "日本語クリップ.mp4");
    CHECK(m->fileSize > 0);
    auto dec = openVideoDecoder(jp);
    REQUIRE_MESSAGE(dec, dec.errorMessage());
    CHECK((*dec)->frameAt(Time::fromSeconds(1.0)));
}

TEST_CASE("frame-accurate random access at 29.97 fps") {
    REQUIRE_MEDIA(ntsc, "ntsc_720p.mp4");
    auto dec = openVideoDecoder(ntsc);
    REQUIRE(dec);
    IVideoDecoder& d = **dec;
    const Rational fps{30000, 1001};
    // Sequential decode establishes the true pts of every frame.
    std::vector<Time> pts;
    for (;;) {
        auto f = d.nextFrame();
        if (!f) break;
        pts.push_back((*f)->pts);
    }
    REQUIRE(pts.size() >= 115);
    for (size_t i = 0; i < pts.size(); ++i) CHECK(pts[i] == Time::fromFrames(static_cast<int64_t>(i), fps));
    // Random access (forward, backward, far seeks) returns the exact frame.
    const int order[] = {100, 3, 57, 58, 58, 20, 119, 0, 90, 45, 46, 10};
    for (int idx : order) {
        if (static_cast<size_t>(idx) >= pts.size()) continue;
        // Request the middle of the frame interval.
        const Time t = Time::fromFrames(idx, fps) + Time::frameDuration(fps).scaled({1, 2});
        auto f = d.frameAt(t);
        REQUIRE(f);
        CHECK_MESSAGE((*f)->pts == Time::fromFrames(idx, fps), "frame ", idx);
    }
    // Requests beyond the end hold the last frame.
    auto last = d.frameAt(Time::fromSeconds(100));
    REQUIRE(last);
    CHECK((*last)->pts == pts.back());
    CHECK(d.stats().seeks > 0);
}

TEST_CASE("decoded pixels are correct (scene colours)") {
    REQUIRE_MEDIA(scenes, "scenes.mp4");
    auto dec = openVideoDecoder(scenes);
    REQUIRE(dec);
    auto red = (*dec)->frameAt(Time::fromSeconds(1.0));
    auto green = (*dec)->frameAt(Time::fromSeconds(3.0));
    auto blue = (*dec)->frameAt(Time::fromSeconds(5.0));
    REQUIRE(red);
    REQUIRE(green);
    REQUIRE(blue);
    auto r = meanRgb(**red), g = meanRgb(**green), b = meanRgb(**blue);
    CHECK(r[0] > 180);
    CHECK(r[1] < 60);
    CHECK(g[1] > 90);
    CHECK(g[0] < 60);
    CHECK(b[2] > 180);
    CHECK(b[0] < 60);
}

TEST_CASE("downscaled decode for thumbnails") {
    REQUIRE_MEDIA(av, "av_1080p30.mp4");
    VideoDecoderOptions opt;
    opt.maxWidth = 160;
    opt.maxHeight = 90;
    auto dec = openVideoDecoder(av, opt);
    REQUIRE(dec);
    auto f = (*dec)->frameAt(Time::fromSeconds(2.0));
    REQUIRE(f);
    CHECK((*f)->width == 160);
    CHECK((*f)->height == 90);
    CHECK((*f)->format == PixelFormat::RGBA8);
}

TEST_CASE("images decode as stills") {
    REQUIRE_MEDIA(png, "still.png");
    auto dec = openVideoDecoder(png);
    REQUIRE(dec);
    auto a = (*dec)->frameAt(Time::fromSeconds(0));
    auto b = (*dec)->frameAt(Time::fromSeconds(123));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->get() == b->get());
    CHECK((*a)->width == 800);
}

TEST_CASE("other codecs decode") {
    for (const char* name : {"vp9.webm", "prores.mov", "mpeg2.ts"}) {
        const std::string path = test::testMedia(name);
        if (path.empty()) continue;
        auto dec = openVideoDecoder(path);
        REQUIRE_MESSAGE(dec, name, dec.errorMessage());
        auto f = (*dec)->frameAt(Time::fromSeconds(1.0));
        CHECK_MESSAGE(f, name);
        if (f) CHECK((*f)->width > 0);
    }
}

TEST_CASE("audio reader is sample accurate and resamples") {
    REQUIRE_MEDIA(wav, "speech_silence.wav");
    auto rd = openAudioReader(wav);
    REQUIRE(rd);
    AudioReader& r = **rd;
    CHECK(r.format().sampleRate == 48000);
    CHECK(r.lengthFrames() == 9 * 48000);
    // Tone at 0.5 s, silence at 1.5 s, tone at 3 s. Mono source upmixed to stereo.
    auto rms = [&](double at) {
        std::vector<float> buf(4800 * 2);
        r.read(static_cast<int64_t>(at * 48000), 4800, buf.data());
        double s = 0;
        for (float v : buf) s += double(v) * v;
        return std::sqrt(s / static_cast<double>(buf.size()));
    };
    CHECK(rms(0.5) > 0.2);
    CHECK(rms(1.5) < 0.001);
    CHECK(rms(3.0) > 0.2);
    CHECK(rms(7.0) < 0.001);
    CHECK(rms(8.5) > 0.2);
    CHECK(rms(-1.0) == 0.0);   // before start
    CHECK(rms(20.0) == 0.0);   // after end
    // Sample-exact edges: tone ends exactly at 1.0 s.
    std::vector<float> edge(200 * 2);
    r.read(48000 - 100, 200, edge.data());
    double before = 0, after = 0;
    for (int i = 0; i < 90; ++i) before += std::fabs(edge[static_cast<size_t>(i) * 2]);
    for (int i = 110; i < 200; ++i) after += std::fabs(edge[static_cast<size_t>(i) * 2]);
    CHECK(before > 1.0);
    CHECK(after < 1e-6);
    // Consistency between random-access and sequential reads.
    std::vector<float> a(1000 * 2), b(1000 * 2);
    r.read(150000, 1000, a.data());
    r.read(10, 1000, b.data());
    r.read(150000, 1000, b.data());
    CHECK(a == b);
}

TEST_CASE("audio resampling 44.1k mp3 to 48k") {
    REQUIRE_MEDIA(mp3, "mono_44k.mp3");
    auto all = decodeAllAudio(mp3, AudioFormat{48000, 1});
    REQUIRE(all);
    CHECK(static_cast<double>(all->size()) / 48000.0 == doctest::Approx(3.0).epsilon(0.02));
    // 660 Hz tone: count zero crossings in 1 s window -> ~1320.
    int crossings = 0;
    for (size_t i = 48000; i < 96000; ++i)
        if (((*all)[i - 1] < 0) != ((*all)[i] < 0)) ++crossings;
    CHECK(crossings == doctest::Approx(1320).epsilon(0.02));
}

TEST_CASE("AAC in mp4 aligns with video timeline") {
    REQUIRE_MEDIA(av, "av_1080p30.mp4");
    auto rd = openAudioReader(av);
    REQUIRE(rd);
    std::vector<float> buf(48000 * 2);
    (*rd)->read(48000, 48000, buf.data());
    int crossings = 0;
    for (size_t i = 2; i < buf.size(); i += 2)
        if ((buf[i - 2] < 0) != (buf[i] < 0)) ++crossings;
    CHECK(crossings == doctest::Approx(880).epsilon(0.02));  // 440 Hz
}

TEST_CASE("frame cache LRU and budget") {
    FrameCache cache(64 * 64 * 4 * 10);
    const uint64_t src = FrameCache::sourceKey("a.mp4", 0, 0);
    for (int i = 0; i < 20; ++i) {
        auto f = allocateFrame(64, 64, PixelFormat::RGBA8);
        f->pts = Time::fromFrames(i, {30, 1});
        f->duration = Time::frameDuration({30, 1});
        cache.insert(src, f);
    }
    auto st = cache.stats();
    CHECK(st.bytes <= st.budget);
    CHECK(st.frames < 20);
    CHECK(cache.find(src, Time::fromFrames(19, {30, 1}) + Time{5}) != nullptr);
    CHECK(cache.find(src, Time::fromFrames(0, {30, 1})) == nullptr);
    cache.trimTo(0);
    CHECK(cache.stats().frames == 0);
}
