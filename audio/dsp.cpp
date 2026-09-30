#include "audio/dsp.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>

#include "effects/effects.h"

namespace avc::audio {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ============================================================ Biquad

void Biquad::set(Type type, double fs, double f0, double q, double gainDb) {
    f0 = std::clamp(f0, 10.0, fs * 0.49);
    q = std::max(q, 0.05);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case Type::LowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case Type::HighPass:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case Type::BandPass:
        b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case Type::Notch:
        b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case Type::Peak:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case Type::LowShelf: {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - s;
        break;
    }
    case Type::HighShelf: {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - s;
        break;
    }
    }
    b0_ = static_cast<float>(b0 / a0);
    b1_ = static_cast<float>(b1 / a0);
    b2_ = static_cast<float>(b2 / a0);
    a1_ = static_cast<float>(a1 / a0);
    a2_ = static_cast<float>(a2 / a0);
}

double Biquad::magnitude(double f, double fs) const {
    const std::complex<double> z = std::polar(1.0, -2.0 * kPi * f / fs);
    const std::complex<double> num = double(b0_) + double(b1_) * z + double(b2_) * z * z;
    const std::complex<double> den = 1.0 + double(a1_) * z + double(a2_) * z * z;
    return std::abs(num / den);
}

void Envelope::setup(double fs, double attack, double release) {
    att_ = static_cast<float>(std::exp(-1.0 / (std::max(attack, 1e-5) * fs)));
    rel_ = static_cast<float>(std::exp(-1.0 / (std::max(release, 1e-5) * fs)));
}

void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / static_cast<double>(len) * (inverse ? 1 : -1);
        const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<float>(n);
}

// ============================================================ effects

namespace {

using ::avc::EffectInstance;

class EffectBase : public fx::IAudioEffect {
public:
    explicit EffectBase(const char* id) : def_(fx::EffectRegistry::instance().find(id)) {}
    void prepare(int sampleRate, int channels) override {
        sr_ = sampleRate;
        ch_ = channels;
        onPrepare();
    }
    void reset() override { onPrepare(); }

protected:
    virtual void onPrepare() {}
    float p(const EffectInstance& inst, const char* id, Time t) const { return def_ ? fx::param1(*def_, inst, id, t) : 0.0f; }
    const fx::EffectDef* def_;
    int sr_ = 48000;
    int ch_ = 2;
};

// ---- filters / EQ
class FilterEffect final : public EffectBase {
public:
    FilterEffect(const char* id, Biquad::Type type) : EffectBase(id), type_(type) {}
    void onPrepare() override { f_.assign(static_cast<size_t>(ch_) * 2, Biquad{}); }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float fc = p(inst, "cutoff", t), q = p(inst, "q", t);
        const int stages = p(inst, "steep", t) > 0.5f ? 2 : 1;
        for (auto& b : f_) b.set(type_, sr_, fc, q);
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < ch_; ++c) {
                float x = s[i * ch_ + c];
                for (int st = 0; st < stages; ++st) x = f_[static_cast<size_t>(c * 2 + st)].process(x);
                s[i * ch_ + c] = x;
            }
    }

private:
    Biquad::Type type_;
    std::vector<Biquad> f_;
};

class EqEffect final : public EffectBase {
public:
    EqEffect() : EffectBase("audio.eq") {}
    void onPrepare() override { bands_.assign(static_cast<size_t>(ch_) * 4, Biquad{}); }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const Biquad::Type types[4] = {Biquad::Type::LowShelf, Biquad::Type::Peak, Biquad::Type::Peak, Biquad::Type::HighShelf};
        const float freq[4] = {p(inst, "lowFreq", t), p(inst, "mid1Freq", t), p(inst, "mid2Freq", t), p(inst, "highFreq", t)};
        const float gain[4] = {p(inst, "lowGain", t), p(inst, "mid1Gain", t), p(inst, "mid2Gain", t), p(inst, "highGain", t)};
        const float q[4] = {0.707f, p(inst, "mid1Q", t), p(inst, "mid2Q", t), 0.707f};
        bool active[4];
        for (int b = 0; b < 4; ++b) {
            active[b] = std::fabs(gain[b]) > 0.01f;
            for (int c = 0; c < ch_; ++c) bands_[static_cast<size_t>(c * 4 + b)].set(types[b], sr_, freq[b], q[b], gain[b]);
        }
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < ch_; ++c) {
                float x = s[i * ch_ + c];
                for (int b = 0; b < 4; ++b)
                    if (active[b]) x = bands_[static_cast<size_t>(c * 4 + b)].process(x);
                s[i * ch_ + c] = x;
            }
    }

private:
    std::vector<Biquad> bands_;
};

// ---- dynamics
class CompressorEffect final : public EffectBase {
public:
    CompressorEffect() : EffectBase("audio.compressor") {}
    void onPrepare() override { env_ = 0.0f; }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float thr = p(inst, "threshold", t), ratio = std::max(1.0f, p(inst, "ratio", t));
        const float knee = std::max(0.0f, p(inst, "knee", t)), makeup = dbToGain(p(inst, "makeup", t));
        const float att = static_cast<float>(std::exp(-1.0 / (std::max(0.0001f, p(inst, "attack", t) / 1000.0f) * sr_)));
        const float rel = static_cast<float>(std::exp(-1.0 / (std::max(0.001f, p(inst, "release", t) / 1000.0f) * sr_)));
        gainReductionDb_ = 0;
        for (int i = 0; i < frames; ++i) {
            float peak = 0;
            for (int c = 0; c < ch_; ++c) peak = std::max(peak, std::fabs(s[i * ch_ + c]));
            const float levelDb = gainToDb(peak);
            // Soft-knee static curve.
            float over = levelDb - thr;
            float grDb = 0;
            if (2 * over <= -knee) grDb = 0;
            else if (2 * std::fabs(over) <= knee && knee > 0) grDb = (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            else grDb = (1 / ratio - 1) * over;
            const float target = -grDb;  // positive reduction
            const float c = target > env_ ? att : rel;
            env_ = c * env_ + (1 - c) * target;
            const float g = dbToGain(-env_) * makeup;
            gainReductionDb_ = std::max(gainReductionDb_, env_);
            for (int ch = 0; ch < ch_; ++ch) s[i * ch_ + ch] *= g;
        }
    }
    float gainReductionDb_ = 0;

private:
    float env_ = 0;
};

class LimiterEffect final : public EffectBase {
public:
    LimiterEffect() : EffectBase("audio.limiter") {}
    void onPrepare() override {
        look_ = std::max(1, sr_ * 5 / 1000);
        delay_.assign(static_cast<size_t>(look_ * ch_), 0.0f);
        peaks_.assign(static_cast<size_t>(look_), 0.0f);
        pos_ = 0;
        gain_ = 1.0f;
    }
    int latencyFrames() const override { return look_; }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float ceiling = dbToGain(p(inst, "ceiling", t));
        const float rel = static_cast<float>(std::exp(-1.0 / (std::max(0.001f, p(inst, "release", t) / 1000.0f) * sr_)));
        for (int i = 0; i < frames; ++i) {
            float peak = 0;
            for (int c = 0; c < ch_; ++c) peak = std::max(peak, std::fabs(s[i * ch_ + c]));
            peaks_[static_cast<size_t>(pos_)] = peak;
            // Maximum over the lookahead window.
            float windowPeak = 0;
            for (float v : peaks_) windowPeak = std::max(windowPeak, v);
            const float target = windowPeak > ceiling ? ceiling / windowPeak : 1.0f;
            if (target < gain_) gain_ = target;  // instant attack (lookahead hides it)
            else gain_ = rel * gain_ + (1 - rel) * target;
            for (int c = 0; c < ch_; ++c) {
                const size_t d = static_cast<size_t>(pos_ * ch_ + c);
                const float delayed = delay_[d];
                delay_[d] = s[i * ch_ + c];
                float y = delayed * gain_;
                y = std::clamp(y, -ceiling, ceiling);  // safety clip
                s[i * ch_ + c] = y;
            }
            pos_ = (pos_ + 1) % look_;
        }
    }

private:
    int look_ = 240, pos_ = 0;
    float gain_ = 1.0f;
    std::vector<float> delay_, peaks_;
};

class GateEffect final : public EffectBase {
public:
    GateEffect() : EffectBase("audio.gate") {}
    void onPrepare() override {
        gain_ = 1.0f;
        hold_ = 0;
        env_ = 0;
    }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float thr = dbToGain(p(inst, "threshold", t));
        const float floorGain = dbToGain(p(inst, "range", t));
        const float att = static_cast<float>(std::exp(-1.0 / (std::max(0.0001f, p(inst, "attack", t) / 1000.0f) * sr_)));
        const float rel = static_cast<float>(std::exp(-1.0 / (std::max(0.001f, p(inst, "release", t) / 1000.0f) * sr_)));
        const int holdSamples = static_cast<int>(p(inst, "hold", t) / 1000.0f * static_cast<float>(sr_));
        const float envCoef = static_cast<float>(std::exp(-1.0 / (0.002 * sr_)));
        for (int i = 0; i < frames; ++i) {
            float peak = 0;
            for (int c = 0; c < ch_; ++c) peak = std::max(peak, std::fabs(s[i * ch_ + c]));
            env_ = std::max(peak, envCoef * env_);
            float target;
            if (env_ >= thr) {
                target = 1.0f;
                hold_ = holdSamples;
            } else if (hold_ > 0) {
                --hold_;
                target = 1.0f;
            } else {
                target = floorGain;
            }
            const float c = target > gain_ ? att : rel;
            gain_ = c * gain_ + (1 - c) * target;
            for (int ch = 0; ch < ch_; ++ch) s[i * ch_ + ch] *= gain_;
        }
    }

private:
    float gain_ = 1, env_ = 0;
    int hold_ = 0;
};

class DeEsserEffect final : public EffectBase {
public:
    DeEsserEffect() : EffectBase("audio.deesser") {}
    void onPrepare() override {
        side_.assign(static_cast<size_t>(ch_), Biquad{});
        env_ = 0;
    }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float freq = p(inst, "frequency", t), thr = p(inst, "threshold", t), maxRed = p(inst, "reduction", t);
        for (auto& b : side_) b.set(Biquad::Type::BandPass, sr_, freq, 1.5);
        const float att = static_cast<float>(std::exp(-1.0 / (0.001 * sr_)));
        const float rel = static_cast<float>(std::exp(-1.0 / (0.06 * sr_)));
        for (int i = 0; i < frames; ++i) {
            float band = 0;
            for (int c = 0; c < ch_; ++c) band = std::max(band, std::fabs(side_[static_cast<size_t>(c)].process(s[i * ch_ + c])));
            const float over = gainToDb(band) - thr;
            const float target = std::clamp(over * 0.75f, 0.0f, maxRed);
            const float c = target > env_ ? att : rel;
            env_ = c * env_ + (1 - c) * target;
            const float g = dbToGain(-env_);
            for (int ch = 0; ch < ch_; ++ch) s[i * ch_ + ch] *= g;
        }
    }

private:
    std::vector<Biquad> side_;
    float env_ = 0;
};

// ---- spectral noise reduction (STFT gating with adaptive noise floor)
class NoiseReductionEffect final : public EffectBase {
public:
    NoiseReductionEffect() : EffectBase("audio.noise_reduction") {}
    static constexpr int N = 1024;
    static constexpr int H = 256;
    int latencyFrames() const override { return N; }
    void onPrepare() override {
        chans_.assign(static_cast<size_t>(ch_), Chan{});
        window_.resize(N);
        for (int i = 0; i < N; ++i) window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / N));
        for (auto& c : chans_) {
            c.in.assign(N, 0.0f);
            c.out.assign(N, 0.0f);
            c.noise.assign(N / 2 + 1, 0.0f);
            c.gains.assign(N / 2 + 1, 1.0f);
            c.smooth.assign(N / 2 + 1, 0.0f);
            c.subMin.assign(N / 2 + 1, 0.0f);
            c.history.assign(static_cast<size_t>(kSubWindows) * (N / 2 + 1), 0.0f);
            c.fill = 0;
            c.slot = 0;
            c.filled = 0;
            c.frames = 0;
        }
    }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float reduction = dbToGain(-p(inst, "reduction", t));
        const float sensitivity = std::clamp(p(inst, "sensitivity", t), 0.0f, 1.0f);
        for (int c = 0; c < ch_; ++c) {
            Chan& st = chans_[static_cast<size_t>(c)];
            for (int i = 0; i < frames; ++i) {
                st.in[static_cast<size_t>(N - H + st.fill)] = s[i * ch_ + c];
                s[i * ch_ + c] = st.out[static_cast<size_t>(st.fill)];
                if (++st.fill == H) {
                    st.fill = 0;
                    processBlock(st, reduction, sensitivity);
                }
            }
        }
    }

private:
    static constexpr int kSubWindow = 16;   // frames per sub-window (~85 ms)
    static constexpr int kSubWindows = 12;  // history length (~1 s)
    struct Chan {
        std::vector<float> in, out, noise, gains, smooth, subMin, history;
        int fill = 0;
        int slot = 0;
        int filled = 0;
        int64_t frames = 0;
    };
    void processBlock(Chan& st, float reduction, float sensitivity) {
        std::vector<std::complex<float>> spec(N);
        for (int i = 0; i < N; ++i) spec[static_cast<size_t>(i)] = st.in[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];
        fft(spec, false);
        // Threshold above the per-bin noise mean: higher sensitivity removes more.
        const float thresholdMul = 1.5f + 2.5f * sensitivity;
        ++st.frames;
        const bool rotate = st.frames % kSubWindow == 0;
        for (int k = 0; k <= N / 2; ++k) {
            const size_t kb = static_cast<size_t>(k);
            const float mag = std::abs(spec[kb]);
            // Minimum statistics: the minimum of the smoothed magnitude over the
            // last ~1 s tracks the noise floor even while someone speaks.
            float& sm = st.smooth[kb];
            sm = st.frames == 1 ? mag : 0.8f * sm + 0.2f * mag;
            float& sub = st.subMin[kb];
            sub = st.frames == 1 ? sm : std::min(sub, sm);
            float m = sub;
            for (int u = 0; u < kSubWindows; ++u)
                if (st.filled > u) m = std::min(m, st.history[static_cast<size_t>(u) * (N / 2 + 1) + kb]);
            if (rotate) {
                st.history[static_cast<size_t>(st.slot) * (N / 2 + 1) + kb] = sub;
                sub = sm;
            }
            const float nf = m * 1.8f;  // bias correction: minimum -> mean
            st.noise[kb] = nf;
            const float over = mag / std::max(nf * thresholdMul, 1e-12f);
            const float target = over >= 1.0f ? 1.0f : std::max(reduction, over * over);
            float& g = st.gains[kb];
            g = target > g ? 0.6f * g + 0.4f * target : 0.8f * g + 0.2f * target;  // smoothing avoids musical noise
            spec[kb] *= g;
            if (k > 0 && k < N / 2) spec[static_cast<size_t>(N - k)] = std::conj(spec[kb]);
        }
        if (rotate) {
            st.slot = (st.slot + 1) % kSubWindows;
            st.filled = std::min(st.filled + 1, kSubWindows);
        }
        fft(spec, true);
        // Overlap-add (Hann^2 with 75% overlap sums to 1.5).
        std::memmove(st.out.data(), st.out.data() + H, (N - H) * sizeof(float));
        std::fill(st.out.begin() + (N - H), st.out.end(), 0.0f);
        for (int i = 0; i < N; ++i)
            st.out[static_cast<size_t>(i)] += spec[static_cast<size_t>(i)].real() * window_[static_cast<size_t>(i)] / 1.5f;
        std::memmove(st.in.data(), st.in.data() + H, (N - H) * sizeof(float));
    }
    std::vector<Chan> chans_;
    std::vector<float> window_;
};

// ---- reverb (Freeverb)
class ReverbEffect final : public EffectBase {
public:
    ReverbEffect() : EffectBase("audio.reverb") {}
    void onPrepare() override {
        static const int combTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int apTuning[4] = {556, 441, 341, 225};
        const double scale = sr_ / 44100.0;
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < 8; ++i) {
                combs_[c][i].buf.assign(static_cast<size_t>((combTuning[i] + c * 23) * scale), 0.0f);
                combs_[c][i].pos = 0;
                combs_[c][i].store = 0;
            }
            for (int i = 0; i < 4; ++i) {
                aps_[c][i].buf.assign(static_cast<size_t>((apTuning[i] + c * 23) * scale), 0.0f);
                aps_[c][i].pos = 0;
            }
        }
    }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float room = p(inst, "roomSize", t) * 0.28f + 0.7f;
        const float damp = p(inst, "damping", t) * 0.4f;
        const float wet = p(inst, "wet", t), dry = p(inst, "dry", t), width = p(inst, "width", t);
        const float wet1 = wet * (width / 2 + 0.5f), wet2 = wet * ((1 - width) / 2);
        for (int i = 0; i < frames; ++i) {
            const float inL = s[i * ch_], inR = ch_ > 1 ? s[i * ch_ + 1] : inL;
            const float input = (inL + inR) * 0.015f;
            float out[2] = {0, 0};
            for (int c = 0; c < 2; ++c) {
                for (auto& cb : combs_[c]) {
                    float y = cb.buf[cb.pos];
                    cb.store = y * (1 - damp) + cb.store * damp;
                    cb.buf[cb.pos] = input + cb.store * room;
                    if (++cb.pos >= cb.buf.size()) cb.pos = 0;
                    out[c] += y;
                }
                for (auto& ap : aps_[c]) {
                    float b = ap.buf[ap.pos];
                    float y = -out[c] + b;
                    ap.buf[ap.pos] = out[c] + b * 0.5f;
                    if (++ap.pos >= ap.buf.size()) ap.pos = 0;
                    out[c] = y;
                }
            }
            s[i * ch_] = inL * dry + out[0] * wet1 + out[1] * wet2;
            if (ch_ > 1) s[i * ch_ + 1] = inR * dry + out[1] * wet1 + out[0] * wet2;
        }
    }

private:
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float store = 0;
    };
    struct AllPass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    std::array<std::array<Comb, 8>, 2> combs_;
    std::array<std::array<AllPass, 4>, 2> aps_;
};

class DelayEffect final : public EffectBase {
public:
    DelayEffect() : EffectBase("audio.delay") {}
    void onPrepare() override {
        buf_.assign(static_cast<size_t>(sr_ * 2 + 16) * static_cast<size_t>(ch_), 0.0f);
        pos_ = 0;
    }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const int len = static_cast<int>(buf_.size() / static_cast<size_t>(ch_));
        const int d = std::clamp(static_cast<int>(p(inst, "time", t) / 1000.0f * static_cast<float>(sr_)), 1, len - 1);
        const float fb = std::clamp(p(inst, "feedback", t), 0.0f, 0.95f), mix = p(inst, "mix", t);
        for (int i = 0; i < frames; ++i) {
            const int rp = (pos_ - d + len) % len;
            for (int c = 0; c < ch_; ++c) {
                const float delayed = buf_[static_cast<size_t>(rp * ch_ + c)];
                const float x = s[i * ch_ + c];
                buf_[static_cast<size_t>(pos_ * ch_ + c)] = x + delayed * fb;
                s[i * ch_ + c] = x * (1 - mix) + delayed * mix;
            }
            pos_ = (pos_ + 1) % len;
        }
    }

private:
    std::vector<float> buf_;
    int pos_ = 0;
};

// Pitch shifter: two crossfaded, modulated delay taps.
class PitchEffect final : public EffectBase {
public:
    PitchEffect() : EffectBase("audio.pitch") {}
    void onPrepare() override {
        win_ = std::max(64, sr_ * 40 / 1000);
        buf_.assign(static_cast<size_t>(win_ * 4) * static_cast<size_t>(ch_), 0.0f);
        pos_ = 0;
        phase_ = 0;
    }
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float ratio = std::pow(2.0f, p(inst, "semitones", t) / 12.0f);
        const float mix = p(inst, "mix", t);
        const int len = win_ * 4;
        const float rate = (1.0f - ratio) / static_cast<float>(win_);  // phase increment per sample
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < ch_; ++c) buf_[static_cast<size_t>(pos_ * ch_ + c)] = s[i * ch_ + c];
            phase_ += rate;
            phase_ -= std::floor(phase_);
            for (int c = 0; c < ch_; ++c) {
                float y = 0;
                for (int tap = 0; tap < 2; ++tap) {
                    float ph = phase_ + 0.5f * static_cast<float>(tap);
                    ph -= std::floor(ph);
                    const float delay = ph * static_cast<float>(win_) + 2.0f;
                    float rp = static_cast<float>(pos_) - delay;
                    while (rp < 0) rp += static_cast<float>(len);
                    const int i0 = static_cast<int>(rp) % len, i1 = (i0 + 1) % len;
                    const float fr = rp - std::floor(rp);
                    const float v = buf_[static_cast<size_t>(i0 * ch_ + c)] * (1 - fr) + buf_[static_cast<size_t>(i1 * ch_ + c)] * fr;
                    const float w = std::sin(static_cast<float>(kPi) * ph);  // crossfade window
                    y += v * w * w;
                }
                s[i * ch_ + c] = s[i * ch_ + c] * (1 - mix) + y * mix;
            }
            pos_ = (pos_ + 1) % len;
        }
    }

private:
    std::vector<float> buf_;
    int win_ = 1920, pos_ = 0;
    float phase_ = 0;
};

class WidthEffect final : public EffectBase {
public:
    WidthEffect() : EffectBase("audio.stereo_width") {}
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        if (ch_ < 2) return;
        const float w = p(inst, "width", t);
        for (int i = 0; i < frames; ++i) {
            const float l = s[i * ch_], r = s[i * ch_ + 1];
            const float m = (l + r) * 0.5f, sd = (l - r) * 0.5f * w;
            s[i * ch_] = m + sd;
            s[i * ch_ + 1] = m - sd;
        }
    }
};

class GainEffect final : public EffectBase {
public:
    GainEffect() : EffectBase("audio.gain") {}
    void process(float* s, int frames, const EffectInstance& inst, Time t) override {
        const float g = dbToGain(p(inst, "gain", t));
        for (int i = 0; i < frames * ch_; ++i) s[i] *= g;
    }
};

fx::EffectDef audioDef(const char* id, const char* name, const char* cat, std::vector<ParamDef> params, fx::AudioFactory f) {
    fx::EffectDef d;
    d.id = id;
    d.name = name;
    d.category = cat;
    d.kind = fx::EffectKind::Audio;
    d.params = std::move(params);
    d.audio = std::move(f);
    return d;
}

ParamDef ap(const char* id, const char* label, float def, float mn, float mx, const char* unit, float step = 0.1f) {
    ParamDef d;
    d.id = id;
    d.label = label;
    d.type = std::string(unit) == "dB" ? ParamType::Decibel : ParamType::Float;
    d.def = pv(def);
    d.minValue = mn;
    d.maxValue = mx;
    d.step = step;
    d.unit = unit;
    return d;
}

}  // namespace

void registerAudioEffects() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& r = fx::EffectRegistry::instance();
        r.add(audioDef("audio.eq", "Parametric EQ", "EQ",
                       {ap("lowFreq", "Low Shelf Freq", 100, 20, 1000, "Hz", 1), ap("lowGain", "Low Shelf Gain", 0, -24, 24, "dB"),
                        ap("mid1Freq", "Mid 1 Freq", 500, 40, 8000, "Hz", 1), ap("mid1Gain", "Mid 1 Gain", 0, -24, 24, "dB"),
                        ap("mid1Q", "Mid 1 Q", 1, 0.1f, 10, "", 0.01f), ap("mid2Freq", "Mid 2 Freq", 3000, 200, 16000, "Hz", 1),
                        ap("mid2Gain", "Mid 2 Gain", 0, -24, 24, "dB"), ap("mid2Q", "Mid 2 Q", 1, 0.1f, 10, "", 0.01f),
                        ap("highFreq", "High Shelf Freq", 8000, 1000, 20000, "Hz", 1), ap("highGain", "High Shelf Gain", 0, -24, 24, "dB")},
                       [] { return std::make_unique<EqEffect>(); }));
        r.add(audioDef("audio.highpass", "High Pass", "EQ",
                       {ap("cutoff", "Cutoff", 80, 20, 5000, "Hz", 1), ap("q", "Q", 0.707f, 0.3f, 5, "", 0.01f),
                        ap("steep", "24 dB/oct", 0, 0, 1, "", 1)},
                       [] { return std::make_unique<FilterEffect>("audio.highpass", Biquad::Type::HighPass); }));
        r.add(audioDef("audio.lowpass", "Low Pass", "EQ",
                       {ap("cutoff", "Cutoff", 12000, 200, 20000, "Hz", 1), ap("q", "Q", 0.707f, 0.3f, 5, "", 0.01f),
                        ap("steep", "24 dB/oct", 0, 0, 1, "", 1)},
                       [] { return std::make_unique<FilterEffect>("audio.lowpass", Biquad::Type::LowPass); }));
        r.add(audioDef("audio.compressor", "Compressor", "Dynamics",
                       {ap("threshold", "Threshold", -18, -60, 0, "dB"), ap("ratio", "Ratio", 4, 1, 20, ":1"),
                        ap("attack", "Attack", 10, 0.1f, 200, "ms"), ap("release", "Release", 120, 10, 2000, "ms", 1),
                        ap("knee", "Knee", 6, 0, 24, "dB"), ap("makeup", "Makeup Gain", 0, 0, 24, "dB")},
                       [] { return std::make_unique<CompressorEffect>(); }));
        r.add(audioDef("audio.limiter", "Limiter", "Dynamics",
                       {ap("ceiling", "Ceiling", -1, -12, 0, "dB"), ap("release", "Release", 50, 1, 1000, "ms", 1)},
                       [] { return std::make_unique<LimiterEffect>(); }));
        r.add(audioDef("audio.gate", "Noise Gate", "Dynamics",
                       {ap("threshold", "Threshold", -45, -90, 0, "dB"), ap("range", "Range", -60, -90, 0, "dB"),
                        ap("attack", "Attack", 1, 0.1f, 100, "ms"), ap("hold", "Hold", 50, 0, 1000, "ms", 1),
                        ap("release", "Release", 150, 5, 2000, "ms", 1)},
                       [] { return std::make_unique<GateEffect>(); }));
        r.add(audioDef("audio.deesser", "De-Esser", "Dynamics",
                       {ap("frequency", "Frequency", 6000, 3000, 12000, "Hz", 10), ap("threshold", "Threshold", -30, -60, 0, "dB"),
                        ap("reduction", "Max Reduction", 8, 0, 24, "dB")},
                       [] { return std::make_unique<DeEsserEffect>(); }));
        r.add(audioDef("audio.noise_reduction", "Noise Reduction", "Restoration",
                       {ap("reduction", "Reduction", 12, 0, 40, "dB"), ap("sensitivity", "Sensitivity", 0.5f, 0, 1, "", 0.01f)},
                       [] { return std::make_unique<NoiseReductionEffect>(); }));
        r.add(audioDef("audio.reverb", "Reverb", "Space",
                       {ap("roomSize", "Room Size", 0.6f, 0, 1, "", 0.01f), ap("damping", "Damping", 0.5f, 0, 1, "", 0.01f),
                        ap("width", "Width", 1, 0, 1, "", 0.01f), ap("wet", "Wet", 0.3f, 0, 1, "", 0.01f),
                        ap("dry", "Dry", 0.8f, 0, 1, "", 0.01f)},
                       [] { return std::make_unique<ReverbEffect>(); }));
        r.add(audioDef("audio.delay", "Delay", "Space",
                       {ap("time", "Time", 300, 1, 2000, "ms", 1), ap("feedback", "Feedback", 0.35f, 0, 0.95f, "", 0.01f),
                        ap("mix", "Mix", 0.3f, 0, 1, "", 0.01f)},
                       [] { return std::make_unique<DelayEffect>(); }));
        r.add(audioDef("audio.pitch", "Pitch Shift", "Pitch",
                       {ap("semitones", "Semitones", 0, -12, 12, "st", 0.1f), ap("mix", "Mix", 1, 0, 1, "", 0.01f)},
                       [] { return std::make_unique<PitchEffect>(); }));
        r.add(audioDef("audio.stereo_width", "Stereo Width", "Space", {ap("width", "Width", 1, 0, 2, "", 0.01f)},
                       [] { return std::make_unique<WidthEffect>(); }));
        r.add(audioDef("audio.gain", "Gain", "Utility", {ap("gain", "Gain", 0, -48, 24, "dB")},
                       [] { return std::make_unique<GainEffect>(); }));
    });
}

}  // namespace avc::audio
