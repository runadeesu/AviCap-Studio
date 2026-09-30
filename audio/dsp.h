#pragma once
// Audio DSP building blocks and the built-in audio effects.

#include <cmath>
#include <complex>
#include <vector>

namespace avc::audio {

inline float dbToGain(float db) { return db <= -120.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return g <= 1e-6f ? -120.0f : 20.0f * std::log10(g); }

// RBJ cookbook biquad (transposed direct form II), one instance per channel.
class Biquad {
public:
    enum class Type { LowPass, HighPass, BandPass, Peak, LowShelf, HighShelf, Notch };
    void set(Type type, double sampleRate, double freq, double q, double gainDb = 0.0);
    float process(float x) {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }
    void reset() { z1_ = z2_ = 0.0f; }
    // Magnitude response at `freq` (for UI curves / tests).
    [[nodiscard]] double magnitude(double freq, double sampleRate) const;

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_ = 0, z2_ = 0;
};

// Envelope follower with separate attack / release (seconds).
class Envelope {
public:
    void setup(double sampleRate, double attack, double release);
    float process(float x) {
        const float c = x > env_ ? att_ : rel_;
        env_ = c * env_ + (1.0f - c) * x;
        return env_;
    }
    void reset() { env_ = 0.0f; }
    [[nodiscard]] float value() const { return env_; }

private:
    float att_ = 0, rel_ = 0, env_ = 0;
};

// In-place radix-2 FFT (size power of two).
void fft(std::vector<std::complex<float>>& data, bool inverse);

// Registers the built-in audio effects (eq, dynamics, reverb, ...) in the
// effect registry. Idempotent.
void registerAudioEffects();

}  // namespace avc::audio
