#include <doctest.h>

#include <chrono>
#include <cmath>
#include <thread>

#include "audio/dsp.h"
#include "audio/mixer.h"
#include "audio/playback.h"
#include "audio/waveform.h"
#include "media/probe.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;
using namespace avc::audio;
using namespace avc::edit;

namespace {

Time S(double s) { return Time::fromSeconds(s); }

std::vector<float> sine(double freq, double amp, int frames, int sr = 48000) {
    std::vector<float> v(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float s = static_cast<float>(amp * std::sin(2 * 3.14159265358979 * freq * i / sr));
        v[static_cast<size_t>(i) * 2] = v[static_cast<size_t>(i) * 2 + 1] = s;
    }
    return v;
}

double rms(const float* s, size_t n, int stride = 1, int offset = 0) {
    double acc = 0;
    size_t cnt = 0;
    for (size_t i = static_cast<size_t>(offset); i < n; i += static_cast<size_t>(stride)) {
        acc += double(s[i]) * s[i];
        ++cnt;
    }
    return cnt ? std::sqrt(acc / static_cast<double>(cnt)) : 0.0;
}

std::unique_ptr<fx::IAudioEffect> makeFx(const std::string& id, EffectInstance& inst) {
    registerAudioEffects();
    auto& reg = fx::EffectRegistry::instance();
    const fx::EffectDef* d = reg.find(id);
    REQUIRE(d);
    inst = reg.instantiate(id);
    auto e = d->audio();
    e->prepare(48000, 2);
    return e;
}

}  // namespace

TEST_CASE("biquad responses") {
    Biquad lp, hp, pk;
    lp.set(Biquad::Type::LowPass, 48000, 1000, 0.707);
    hp.set(Biquad::Type::HighPass, 48000, 1000, 0.707);
    pk.set(Biquad::Type::Peak, 48000, 1000, 1.0, 6.0);
    CHECK(lp.magnitude(100, 48000) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(lp.magnitude(1000, 48000) == doctest::Approx(0.707).epsilon(0.02));
    CHECK(lp.magnitude(10000, 48000) < 0.02);
    CHECK(hp.magnitude(10000, 48000) == doctest::Approx(1.0).epsilon(0.02));
    CHECK(gainToDb(static_cast<float>(pk.magnitude(1000, 48000))) == doctest::Approx(6.0).epsilon(0.02));
}

TEST_CASE("fft roundtrip") {
    std::vector<std::complex<float>> a(256);
    for (size_t i = 0; i < a.size(); ++i) a[i] = std::sin(0.3f * static_cast<float>(i)) + 0.1f * static_cast<float>(i % 7);
    auto orig = a;
    fft(a, false);
    fft(a, true);
    for (size_t i = 0; i < a.size(); ++i) CHECK(std::abs(a[i] - orig[i]) < 1e-4f);
}

TEST_CASE("dynamics compressor limiter gate") {
    EffectInstance inst;
    auto comp = makeFx("audio.compressor", inst);
    inst.params.setStatic("threshold", pv(-20));
    inst.params.setStatic("ratio", pv(10));
    inst.params.setStatic("knee", pv(0));
    auto loud = sine(440, 0.9, 48000);
    const double before = rms(loud.data(), loud.size());
    comp->process(loud.data(), 48000, inst, Time{0});
    const double after = rms(loud.data() + 48000, 48000);
    CHECK(gainToDb(static_cast<float>(after)) < gainToDb(static_cast<float>(before)) - 10);

    EffectInstance li;
    auto lim = makeFx("audio.limiter", li);
    li.params.setStatic("ceiling", pv(-3));
    auto hot = sine(200, 1.5, 48000);
    lim->process(hot.data(), 48000, li, Time{0});
    float peak = 0;
    for (float v : hot) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= dbToGain(-3) + 1e-4f);
    CHECK(lim->latencyFrames() > 0);

    EffectInstance gi;
    auto gate = makeFx("audio.gate", gi);
    gi.params.setStatic("threshold", pv(-30));
    gi.params.setStatic("range", pv(-60));
    auto quiet = sine(300, 0.005, 48000);  // -46 dBFS, below threshold
    gate->process(quiet.data(), 48000, gi, Time{0});
    CHECK(rms(quiet.data() + 48000, 48000) < 0.0005);
}

TEST_CASE("eq filters reverb delay pitch width and noise reduction") {
    registerAudioEffects();
    for (const char* id : {"audio.eq", "audio.highpass", "audio.lowpass", "audio.deesser", "audio.noise_reduction",
                           "audio.reverb", "audio.delay", "audio.pitch", "audio.stereo_width", "audio.gain"}) {
        EffectInstance inst;
        auto e = makeFx(id, inst);
        auto s = sine(1000, 0.3, 9600);
        e->process(s.data(), 9600, inst, Time{0});
        bool finite = true;
        for (float v : s) finite = finite && std::isfinite(v);
        CHECK_MESSAGE(finite, id);
    }
    // High-pass removes low frequencies.
    EffectInstance hi;
    auto hp = makeFx("audio.highpass", hi);
    hi.params.setStatic("cutoff", pv(2000));
    auto low = sine(100, 0.5, 48000);
    hp->process(low.data(), 48000, hi, Time{0});
    CHECK(rms(low.data() + 48000, 48000) < 0.02);
    // Reverb produces a tail after the input stops.
    EffectInstance ri;
    auto rv = makeFx("audio.reverb", ri);
    std::vector<float> imp(48000 * 2, 0.0f);
    imp[0] = imp[1] = 1.0f;
    rv->process(imp.data(), 48000, ri, Time{0});
    CHECK(rms(imp.data() + 20000, 20000) > 1e-5);
    // Pitch +12 doubles the dominant frequency (zero crossings).
    EffectInstance pi;
    auto pt = makeFx("audio.pitch", pi);
    pi.params.setStatic("semitones", pv(12));
    auto tone = sine(220, 0.5, 48000);
    pt->process(tone.data(), 48000, pi, Time{0});
    int zc = 0;
    for (size_t i = 24000 + 2; i < 96000; i += 2)
        if ((tone[i - 2] < 0) != (tone[i] < 0)) ++zc;
    CHECK(zc == doctest::Approx(440 * 2 * 0.75).epsilon(0.15));
    // Noise reduction attenuates steady broadband noise.
    EffectInstance ni;
    auto nr = makeFx("audio.noise_reduction", ni);
    ni.params.setStatic("reduction", pv(24));
    std::vector<float> noise(48000 * 8);
    uint32_t seed = 1;
    for (auto& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.05f;
    }
    const double nb = rms(noise.data(), noise.size());
    nr->process(noise.data(), 48000 * 4, ni, Time{0});
    CHECK(rms(noise.data() + 48000 * 4, 48000 * 4) < nb * 0.5);  // after ~1 s of learning
    // Speech-like bursts well above the noise survive almost untouched.
    EffectInstance ni2;
    auto nr2 = makeFx("audio.noise_reduction", ni2);
    auto toneNoise = sine(440, 0.3, 48000 * 4);
    for (size_t i = 0; i < toneNoise.size(); i += 2)
        if ((i / 2 / 14400) % 2 == 1) toneNoise[i] = toneNoise[i + 1] = 0.0f;  // 0.3 s on / 0.3 s off
    auto clean = toneNoise;
    seed = 7;
    for (auto& v : toneNoise) {
        seed = seed * 1664525u + 1013904223u;
        v += (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.02f;
    }
    const double tn = rms(clean.data() + 48000 * 4, 48000 * 4);
    nr2->process(toneNoise.data(), 48000 * 4, ni2, Time{0});
    CHECK(rms(toneNoise.data() + 48000 * 4, 48000 * 4) > tn * 0.9);
}

namespace {
struct MixFixture {
    Document doc{std::make_shared<Project>(makeProject("A"))};
    SequenceId seq() const { return doc.project().activeSequence; }
    const Sequence& s() const { return *doc.project().active(); }
    MediaId import(const std::string& path) {
        auto m = createMediaItem(path);
        REQUIRE(m);
        const MediaId id = m->id;
        REQUIRE(doc.edit("i", [&](ProjectEditor& pe) {
            pe.addMedia(*m);
            return Status::ok();
        }));
        return id;
    }
    std::vector<ClipId> place(MediaId m, Time at, int audioTrack) {
        std::vector<ClipId> ids;
        REQUIRE(doc.editSequence("p", seq(), [&](SequenceEditor& e) {
            auto r = addMediaClip(e, *doc.project().findMedia(m), at, -1, audioTrack, PlaceMode::Overwrite);
            if (r) ids = *r;
            return r.status();
        }));
        return ids;
    }
    std::vector<float> mix(double start, double dur) { return mixdown(doc.current(), s(), TimeRange{S(start), S(dur)}); }
};
}  // namespace

TEST_CASE("mixer placement volume pan mute") {
    const std::string wav = test::testMedia("speech_silence.wav");
    if (wav.empty()) return;
    MixFixture f;
    auto m = f.import(wav);
    auto a = f.place(m, S(2), 3);
    // Before the clip: silence. Inside the tone region (clip 0.5s -> timeline 2.5s): signal.
    auto pre = f.mix(0, 1.0);
    CHECK(rms(pre.data(), pre.size()) < 1e-6);
    auto on = f.mix(2.2, 0.5);
    const double base = rms(on.data(), on.size());
    CHECK(base > 0.2);
    // -6 dB volume halves amplitude.
    REQUIRE(f.doc.editSequence("v", f.seq(), [&](SequenceEditor& e) {
        e.mutableClip(a[0]).audio.setStatic("volume", pv(-6.0206f));
        return Status::ok();
    }));
    auto quieter = f.mix(2.2, 0.5);
    CHECK(rms(quieter.data(), quieter.size()) == doctest::Approx(base / 2).epsilon(0.02));
    // Full-left pan silences the right channel.
    REQUIRE(f.doc.editSequence("p", f.seq(), [&](SequenceEditor& e) {
        e.mutableClip(a[0]).audio.setStatic("pan", pv(-1));
        return Status::ok();
    }));
    auto panned = f.mix(2.2, 0.5);
    CHECK(rms(panned.data(), panned.size(), 2, 1) < 1e-6);
    CHECK(rms(panned.data(), panned.size(), 2, 0) > 0.05);
    // Track mute.
    REQUIRE(f.doc.editSequence("m", f.seq(), [&](SequenceEditor& e) {
        e.mutableTrack(3).muted = true;
        return Status::ok();
    }));
    auto muted = f.mix(2.2, 0.5);
    CHECK(rms(muted.data(), muted.size()) < 1e-6);
}

TEST_CASE("mixer sample accurate edits and speed") {
    const std::string wav = test::testMedia("speech_silence.wav");
    if (wav.empty()) return;
    MixFixture f;
    auto m = f.import(wav);
    f.place(m, S(0), 3);
    // Mixing at the timeline position equals the file content exactly.
    auto direct = decodeAllAudio(wav, AudioFormat{48000, 2});
    REQUIRE(direct);
    auto mixed = f.mix(0.25, 0.5);
    for (size_t i = 0; i < mixed.size(); i += 97) CHECK(mixed[i] == doctest::Approx((*direct)[12000 * 2 + i]).epsilon(1e-5));
    // Speed 2x: tone region shortens (0-1s source -> 0-0.5s timeline).
    const ClipId cid = f.s().tracks[3]->clips[0]->id;
    REQUIRE(f.doc.editSequence("s", f.seq(), [&](SequenceEditor& e) { return setClipSpeed(e, cid, {2, 1}, true); }));
    auto fast = f.mix(0.6, 0.3);  // source 1.2-1.8 s is silent
    CHECK(rms(fast.data(), fast.size()) < 0.01);
    auto fastTone = f.mix(0.1, 0.3);
    int zc = 0;
    for (size_t i = 2; i < fastTone.size(); i += 2)
        if ((fastTone[i - 2] < 0) != (fastTone[i] < 0)) ++zc;
    CHECK(zc == doctest::Approx(0.3 * 600 * 2).epsilon(0.05));  // 300 Hz doubled to 600 Hz
}

TEST_CASE("waveform peaks and LOD query") {
    auto s = sine(100, 0.5, 48000 * 3);
    // Silence the middle second.
    for (size_t i = 48000 * 2; i < 96000 * 2; ++i) s[i] = 0;
    auto w = WaveformPeaks::fromSamples(s.data(), 48000 * 3, 2, 48000);
    CHECK(w.levels.size() > 5);
    auto cols = w.query(0, 3, 30);
    REQUIRE(cols.size() == 30);
    CHECK(cols[5].second > 0.45f);
    CHECK(cols[5].first < -0.45f);
    CHECK(std::fabs(cols[15].second) < 0.01f);
    auto round = WaveformPeaks::deserialize(w.serialize());
    REQUIRE(round);
    CHECK(round->levels.size() == w.levels.size());
    CHECK(round->query(0, 3, 30)[5].second == cols[5].second);
    const std::string wav = test::testMedia("speech_silence.wav");
    if (!wav.empty()) {
        auto a = analyzeWaveform(wav);
        REQUIRE(a);
        CHECK(a->durationSeconds() == doctest::Approx(9.0).epsilon(0.01));
        auto q = a->query(0, 9, 9);
        CHECK(q[0].second > 0.45f);  // mono sources are upmixed at unity
        auto silent = a->query(6.3, 7.7, 1);
        CHECK(std::fabs(silent[0].second) < 0.01f);
    }
}

TEST_CASE("playback clock advances in real time and stops at end") {
    Document doc(std::make_shared<Project>(makeProject("P")));
    Clip solid = makeSolidClip(pv(1, 1, 1, 1), S(0), S(0.6));
    REQUIRE(doc.editSequence("p", doc.project().activeSequence, [&](SequenceEditor& e) {
        return placeClips(e, {{0, solid}}, PlaceMode::Overwrite);
    }));
    PlaybackEngine engine([&] { return std::make_pair(doc.snapshot(), doc.project().activeSequence); },
                          createNullOutput(48000, 480));
    engine.setEndTime(S(0.6));
    engine.seek(S(0.1));
    CHECK(engine.position() == S(0.1));
    engine.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double p = engine.position().seconds();
    CHECK(p > 0.25);
    CHECK(p < 0.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_FALSE(engine.playing());
    CHECK(engine.position().seconds() == doctest::Approx(0.6).epsilon(0.05));
    // Reverse shuttle at 2x moves backwards.
    engine.seek(S(0.5));
    engine.play(-2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(engine.position().seconds() < 0.45);
    engine.pause();
}
