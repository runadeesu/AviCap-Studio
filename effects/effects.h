#pragma once
// Effect and transition registry.
//
// Every effect is registered with its parameter definitions (UI metadata,
// defaults, keyframability) and an implementation. Video effects are pass
// graphs over gpu::Device kernels so the same code serves preview and export.
// Audio effects implement IAudioEffect (registered by the audio module).
// Plugins add entries through the same registry (see plugins/).

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "render/gpu/gpu.h"
#include "timeline/model.h"

namespace avc::fx {

enum class EffectKind { Video, Audio };

struct VideoEnv {
    gpu::Device& device;
    gpu::TexturePool& pool;
    float pixelScale = 1.0f;  // layer pixels per nominal (full quality) pixel
    Time localTime;           // clip-local time for keyframes
    double sequenceSeconds = 0;
    Rational frameRate{30, 1};
    int sequenceWidth = 1920;
    int sequenceHeight = 1080;
};

using VideoApply = std::function<gpu::TexturePtr(VideoEnv&, const EffectInstance&, const gpu::TexturePtr& input)>;

// Real-time audio processing interface (interleaved float, in place).
class IAudioEffect {
public:
    virtual ~IAudioEffect() = default;
    virtual void prepare(int sampleRate, int channels) = 0;
    // `params` evaluated for the block start (clip-local time).
    virtual void process(float* samples, int frames, const EffectInstance& params, Time localTime) = 0;
    virtual void reset() = 0;
    // Latency introduced (lookahead limiters), in frames.
    [[nodiscard]] virtual int latencyFrames() const { return 0; }
};
using AudioFactory = std::function<std::unique_ptr<IAudioEffect>()>;

struct EffectDef {
    std::string id;
    std::string name;       // English display name (translated by UI)
    std::string category;   // "Blur", "Color", "Stylize", ...
    EffectKind kind = EffectKind::Video;
    std::vector<ParamDef> params;
    VideoApply apply;       // video
    AudioFactory audio;     // audio
    std::string description;
};

class EffectRegistry {
public:
    static EffectRegistry& instance();
    void add(EffectDef def);  // replaces an existing id
    [[nodiscard]] const EffectDef* find(const std::string& id) const;
    [[nodiscard]] std::vector<const EffectDef*> list(EffectKind kind) const;
    // New instance with every parameter at its default.
    [[nodiscard]] EffectInstance instantiate(const std::string& id) const;

private:
    EffectRegistry();
    std::deque<EffectDef> defs_;  // deque: stable addresses when plugins add entries
};

// Evaluated parameter (falls back to the definition default).
ParamValue paramValue(const EffectDef& def, const EffectInstance& inst, const std::string& id, Time t);
float param1(const EffectDef& def, const EffectInstance& inst, const std::string& id, Time t);

struct TransitionDef {
    std::string id;
    std::string name;
    std::string category;
    int kernelType = 0;
    ParamValue color{0, 0, 0, 1};
    std::vector<ParamDef> params;  // direction, softness
    double defaultSeconds = 1.0;
};
const std::vector<TransitionDef>& transitions();
const TransitionDef* findTransition(const std::string& id);

// Runs a transition kernel over two full-frame images.
gpu::TexturePtr applyTransition(gpu::Device& dev, gpu::TexturePool& pool, const TransitionSpec& spec, float progress,
                                const gpu::TexturePtr& a, const gpu::TexturePtr& b, int width, int height);

// Shared building blocks used by effects, text and the compositor.
gpu::TexturePtr gaussianBlur(gpu::Device& dev, gpu::TexturePool& pool, const gpu::TexturePtr& src, float sigmaPx);
gpu::TexturePtr resample(gpu::Device& dev, gpu::TexturePool& pool, const gpu::TexturePtr& src, int width, int height);

// Parses a .cube 3D LUT file into a packed (N*N) x N RGBA32F image.
struct Lut3D {
    int size = 0;
    std::vector<float> rgba;  // (size*size) * size * 4
};
std::shared_ptr<const Lut3D> loadCubeLut(const std::string& utf8Path, std::string* error = nullptr);

// Curve lookup (0..1 -> 0..1) from control points "x,y;x,y;..." (monotone cubic).
std::vector<float> buildCurveTable(const std::string& points, int samples = 256, bool centeredAdjust = false);

}  // namespace avc::fx
