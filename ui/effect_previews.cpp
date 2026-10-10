#include "ui/effect_previews.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "effects/effects.h"
#include "render/gpu/gpu.h"

namespace avc::ui {

namespace {

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float smooth(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
struct Rgb {
    float r, g, b;
};
Rgb lerp(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// Showcase settings for effects that are neutral at their defaults, so the
// tile shows a typical use.
void showcase(EffectInstance& inst) {
    const std::string& id = inst.effectId;
    if (id == "blur.gaussian") inst.params.setStatic("radius", pv(40.0f));
    else if (id == "blur.directional") inst.params.setStatic("length", pv(90.0f));
    else if (id == "blur.motion") {
        inst.params.setStatic("angle", pv(20.0f));
        inst.params.setStatic("length", pv(90.0f));
    } else if (id == "blur.radial") inst.params.setStatic("amount", pv(0.4f));
    else if (id == "sharpen") inst.params.setStatic("amount", pv(3.0f));
    else if (id == "distort.chromatic_aberration") inst.params.setStatic("strength", pv(0.05f));
    else if (id == "color.basic") {
        inst.params.setStatic("saturation", pv(1.7f));
        inst.params.setStatic("contrast", pv(0.3f));
        inst.params.setStatic("temperature", pv(0.35f));
        inst.params.setStatic("vibrance", pv(0.3f));
    }
}

gpu::TexturePtr uploadImage(gpu::Device& dev, gpu::TexturePool& pool, const cache::Image& img) {
    gpu::TexturePtr tex = pool.acquire(img.width, img.height);
    if (!tex) return nullptr;
    std::vector<uint16_t> halfs(img.rgba.size());
    for (size_t i = 0; i < img.rgba.size(); ++i) halfs[i] = gpu::floatToHalf(static_cast<float>(img.rgba[i]) / 255.0f);
    dev.upload(*tex, halfs.data(), img.width * 8);
    return tex;
}

// Reads the result back and flattens transparency over a checkerboard.
cache::ImagePtr toImage(gpu::Device& dev, const gpu::Texture& tex, bool checker = true) {
    auto img = std::make_shared<cache::Image>();
    img->width = tex.width();
    img->height = tex.height();
    img->rgba = gpu::readbackRgba8(dev, tex, true);
    const int cell = std::max(4, img->height / 9);
    for (int y = 0; y < img->height; ++y)
        for (int x = 0; x < img->width; ++x) {
            uint8_t* p = &img->rgba[(static_cast<size_t>(y) * static_cast<size_t>(img->width) + static_cast<size_t>(x)) * 4];
            const float a = p[3] / 255.0f;
            const float bg = !checker ? 0.0f : ((x / cell + y / cell) % 2) ? 70.0f : 46.0f;
            for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::lround(p[c] * a + bg * (1 - a)));
            p[3] = 255;
        }
    return img;
}

bool looksUnchanged(const cache::Image& a, const cache::Image& b) {
    if (a.width != b.width || a.height != b.height) return false;
    double diff = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c) diff += std::abs(static_cast<int>(a.rgba[i + static_cast<size_t>(c)]) - static_cast<int>(b.rgba[i + static_cast<size_t>(c)]));
    return diff / (static_cast<double>(a.width) * a.height * 3) < 1.0;
}

}  // namespace

cache::ImagePtr effectPreviewSample(int which, int w, int h) {
    auto img = std::make_shared<cache::Image>();
    img->width = w;
    img->height = h;
    img->rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const float W = static_cast<float>(w), H = static_cast<float>(h);
    for (int yi = 0; yi < h; ++yi)
        for (int xi = 0; xi < w; ++xi) {
            const float x = (static_cast<float>(xi) + 0.5f) / W, y = (static_cast<float>(yi) + 0.5f) / H;
            Rgb c{};
            if (which == 0) {
                // Sunset: sky gradient, sun with glow, two mountain ranges, green field.
                c = lerp(Rgb{0.16f, 0.27f, 0.62f}, Rgb{0.98f, 0.58f, 0.34f}, smooth(0.0f, 0.62f, y));
                const float dx = (x - 0.68f) * W / H, dy = y - 0.40f;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float glow = std::exp(-d * 9.0f) * 0.55f;
                c = {c.r + glow, c.g + glow * 0.8f, c.b + glow * 0.45f};
                if (d < 0.085f) c = lerp(c, Rgb{1.0f, 0.95f, 0.80f}, smooth(0.085f, 0.075f, d));
                const float back = 0.52f + 0.07f * std::sin(x * 9.0f) + 0.04f * std::sin(x * 23.0f + 1.0f);
                if (y > back) c = lerp(Rgb{0.42f, 0.33f, 0.58f}, Rgb{0.30f, 0.24f, 0.45f}, smooth(back, back + 0.2f, y));
                const float front = 0.62f + 0.05f * std::sin(x * 13.0f + 2.0f) + 0.02f * std::sin(x * 41.0f);
                if (y > front) c = lerp(Rgb{0.22f, 0.18f, 0.36f}, Rgb{0.16f, 0.13f, 0.28f}, smooth(front, front + 0.15f, y));
                const float field = 0.74f + 0.015f * std::sin(x * 7.0f);
                if (y > field) {
                    c = lerp(Rgb{0.18f, 0.62f, 0.30f}, Rgb{0.08f, 0.40f, 0.20f}, smooth(field, 1.0f, y));
                    if (std::fmod(x * 18.0f + y * 6.0f, 1.0f) < 0.08f) c = lerp(c, Rgb{0.30f, 0.75f, 0.38f}, 0.6f);
                }
            } else {
                // Night city: violet sky, moon, skyline with lit windows.
                c = lerp(Rgb{0.06f, 0.04f, 0.16f}, Rgb{0.42f, 0.14f, 0.46f}, smooth(0.0f, 0.75f, y));
                const float dx = (x - 0.25f) * W / H, dy = y - 0.24f;
                if (std::sqrt(dx * dx + dy * dy) < 0.07f) c = Rgb{0.96f, 0.94f, 0.86f};
                const int bld = static_cast<int>(x * 14.0f);
                const float top = 0.45f + 0.25f * std::fabs(std::sin(static_cast<float>(bld) * 2.3f + 0.7f));
                if (y > top) {
                    c = Rgb{0.08f, 0.07f, 0.14f};
                    const float wx = std::fmod(x * 14.0f, 1.0f), wy = std::fmod((y - top) * 40.0f, 1.0f);
                    const bool lit = (static_cast<int>(y * 40.0f) * 7 + bld * 3) % 5 < 2;
                    if (wx > 0.2f && wx < 0.8f && wy > 0.3f && wy < 0.75f && lit) c = Rgb{1.0f, 0.78f, 0.36f};
                }
                if (y > 0.9f) c = lerp(Rgb{0.12f, 0.10f, 0.22f}, Rgb{0.85f, 0.35f, 0.55f}, 0.25f * std::fabs(std::sin(x * 30.0f)));
            }
            uint8_t* p = &img->rgba[(static_cast<size_t>(yi) * static_cast<size_t>(w) + static_cast<size_t>(xi)) * 4];
            p[0] = static_cast<uint8_t>(std::lround(clamp01(c.r) * 255));
            p[1] = static_cast<uint8_t>(std::lround(clamp01(c.g) * 255));
            p[2] = static_cast<uint8_t>(std::lround(clamp01(c.b) * 255));
            p[3] = 255;
        }
    return img;
}

std::map<std::string, cache::ImagePtr> renderEffectPreviews(int w, int h, const CancelToken& cancel) {
    std::map<std::string, cache::ImagePtr> out;
    std::unique_ptr<gpu::Device> dev = gpu::createCpuDevice(1);
    if (!dev) return out;
    gpu::TexturePool pool(*dev);
    const cache::ImagePtr sampleA = effectPreviewSample(0, w, h);
    const cache::ImagePtr sampleB = effectPreviewSample(1, w, h);
    const gpu::TexturePtr a = uploadImage(*dev, pool, *sampleA);
    const gpu::TexturePtr b = uploadImage(*dev, pool, *sampleB);
    if (!a || !b) return out;
    const auto& reg = fx::EffectRegistry::instance();
    for (const fx::EffectDef* def : reg.list(fx::EffectKind::Video)) {
        if (cancel.cancelled()) return out;
        if (!def->apply) continue;
        EffectInstance inst = reg.instantiate(def->id);
        showcase(inst);
        fx::VideoEnv env{*dev, pool};
        env.pixelScale = static_cast<float>(h) / 1080.0f * 2.5f;  // exaggerated a little so it reads on a thumbnail
        env.localTime = Time::fromSeconds(0.5);
        env.sequenceSeconds = 0.5;
        env.sequenceWidth = w;
        env.sequenceHeight = h;
        gpu::TexturePtr result;
        try {
            result = def->apply(env, inst, a);
        } catch (const std::exception& e) {
            AVC_WARN("ui", "effect preview {} failed: {}", def->id, e.what());
        }
        if (!result) {
            out[def->id] = nullptr;
            continue;
        }
        cache::ImagePtr img = toImage(*dev, *result);
        out[def->id] = looksUnchanged(*img, *sampleA) ? nullptr : img;
    }
    for (const fx::TransitionDef& t : fx::transitions()) {
        if (cancel.cancelled()) return out;
        TransitionSpec spec;
        spec.type = t.id;
        spec.duration = Time::fromSeconds(t.defaultSeconds);
        gpu::TexturePtr result = fx::applyTransition(*dev, pool, spec, 0.4f, a, b, w, h);
        out["transition:" + t.id] = result ? toImage(*dev, *result, false) : nullptr;
    }
    return out;
}

}  // namespace avc::ui
