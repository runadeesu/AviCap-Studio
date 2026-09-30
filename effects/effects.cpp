#include "effects/effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>

#include "core/file_io.h"
#include "core/log.h"
#include "core/strings.h"

namespace avc::fx {

using gpu::Kernel;
using gpu::KernelParams;
using gpu::TexturePtr;

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

ParamDef pf(const char* id, const char* label, float def, float mn, float mx, const char* unit = "", float step = 0.01f,
            ParamType type = ParamType::Float) {
    ParamDef d;
    d.id = id;
    d.label = label;
    d.type = type;
    d.def = pv(def);
    d.minValue = mn;
    d.maxValue = mx;
    d.step = step;
    d.unit = unit;
    return d;
}
ParamDef pvec(const char* id, const char* label, float x, float y, float mn, float mx, const char* unit = "") {
    ParamDef d = pf(id, label, 0, mn, mx, unit, 1.0f, ParamType::Vec2);
    d.def = pv(x, y);
    return d;
}
ParamDef pcol(const char* id, const char* label, float r, float g, float b, float a = 1.0f) {
    ParamDef d = pf(id, label, 0, 0, 1, "", 0.01f, ParamType::Color);
    d.def = pv(r, g, b, a);
    return d;
}
ParamDef penum(const char* id, const char* label, std::vector<std::string> values, int def = 0) {
    ParamDef d = pf(id, label, static_cast<float>(def), 0, static_cast<float>(values.size() - 1), "", 1.0f, ParamType::Enum);
    d.enumLabels = std::move(values);
    d.keyframable = false;
    return d;
}
ParamDef pbool(const char* id, const char* label, bool def) {
    ParamDef d = pf(id, label, def ? 1.0f : 0.0f, 0, 1, "", 1.0f, ParamType::Bool);
    d.keyframable = false;
    return d;
}

TexturePtr sameSize(gpu::TexturePool& pool, const TexturePtr& t) { return pool.acquire(t->width(), t->height()); }

}  // namespace

// ============================================================ helpers

TexturePtr resample(gpu::Device& dev, gpu::TexturePool& pool, const TexturePtr& src, int w, int h) {
    TexturePtr out = pool.acquire(std::max(1, w), std::max(1, h));
    KernelParams p;
    p.set(0, 1, 1, 0, 0);
    dev.run(Kernel::Copy, p, {src.get()}, *out);
    return out;
}

TexturePtr gaussianBlur(gpu::Device& dev, gpu::TexturePool& pool, const TexturePtr& src, float sigma) {
    if (sigma < 0.3f) return src;
    // Downsample for large radii so tap counts stay bounded.
    int factor = 1;
    while (sigma / static_cast<float>(factor) > 12.0f && src->width() / (factor * 2) >= 8 && src->height() / (factor * 2) >= 8)
        factor *= 2;
    TexturePtr work = src;
    if (factor > 1) {
        // Progressive halving keeps the downsample alias-free.
        int w = src->width(), h = src->height();
        for (int f = 1; f < factor; f *= 2) {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            work = resample(dev, pool, work, w, h);
        }
    }
    const float s = sigma / static_cast<float>(factor);
    const float radius = std::min(96.0f, std::ceil(s * 3.0f));
    TexturePtr tmp = sameSize(pool, work), out = sameSize(pool, work);
    KernelParams h, v;
    h.set(0, 1.0f / static_cast<float>(work->width()), 0, s, radius);
    v.set(0, 0, 1.0f / static_cast<float>(work->height()), s, radius);
    dev.run(Kernel::BlurGaussian, h, {work.get()}, *tmp);
    dev.run(Kernel::BlurGaussian, v, {tmp.get()}, *out);
    if (factor > 1) out = resample(dev, pool, out, src->width(), src->height());
    return out;
}

std::vector<float> buildCurveTable(const std::string& text, int samples, bool centered) {
    std::vector<std::pair<float, float>> pts;
    for (const auto& part : split(text, ';')) {
        const auto xy = split(part, ',');
        if (xy.size() != 2) continue;
        pts.emplace_back(std::clamp(std::strtof(xy[0].c_str(), nullptr), 0.0f, 1.0f),
                         std::clamp(std::strtof(xy[1].c_str(), nullptr), 0.0f, 1.0f));
    }
    std::sort(pts.begin(), pts.end());
    if (pts.empty()) {
        if (centered) pts = {{0.0f, 0.5f}, {1.0f, 0.5f}};
        else pts = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    }
    if (pts.front().first > 0.0f) pts.insert(pts.begin(), {0.0f, pts.front().second});
    if (pts.back().first < 1.0f) pts.emplace_back(1.0f, pts.back().second);
    // Fritsch-Carlson monotone cubic Hermite.
    const size_t n = pts.size();
    std::vector<float> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i) {
        const float dx = std::max(1e-6f, pts[i + 1].first - pts[i].first);
        d[i] = (pts[i + 1].second - pts[i].second) / dx;
    }
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0.0f : 0.5f * (d[i - 1] + d[i]);
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0f) {
            m[i] = m[i + 1] = 0.0f;
            continue;
        }
        const float a = m[i] / d[i], b = m[i + 1] / d[i];
        const float s = a * a + b * b;
        if (s > 9.0f) {
            const float t = 3.0f / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    std::vector<float> table(static_cast<size_t>(samples));
    size_t seg = 0;
    for (int i = 0; i < samples; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(samples - 1);
        while (seg + 2 < n && x > pts[seg + 1].first) ++seg;
        const float x0 = pts[seg].first, x1 = pts[seg + 1].first;
        const float h = std::max(1e-6f, x1 - x0);
        const float t = std::clamp((x - x0) / h, 0.0f, 1.0f);
        const float t2 = t * t, t3 = t2 * t;
        const float y = (2 * t3 - 3 * t2 + 1) * pts[seg].second + (t3 - 2 * t2 + t) * h * m[seg] +
                        (-2 * t3 + 3 * t2) * pts[seg + 1].second + (t3 - t2) * h * m[seg + 1];
        table[static_cast<size_t>(i)] = std::clamp(y, 0.0f, 1.0f);
    }
    return table;
}

std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* error) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const Lut3D>> cache;
    {
        std::lock_guard lock(m);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
    }
    auto data = readFileBytes(pathFromUtf8(path));
    if (!data) {
        if (error) *error = "Cannot read LUT file";
        return nullptr;
    }
    auto lut = std::make_shared<Lut3D>();
    std::istringstream in(*data);
    std::string line;
    std::vector<float> values;
    float domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (startsWith(t, "LUT_3D_SIZE")) {
            lut->size = std::atoi(t.c_str() + 11);
        } else if (startsWith(t, "DOMAIN_MIN")) {
            std::sscanf(t.c_str() + 10, "%f %f %f", &domainMin[0], &domainMin[1], &domainMin[2]);
        } else if (startsWith(t, "DOMAIN_MAX")) {
            std::sscanf(t.c_str() + 10, "%f %f %f", &domainMax[0], &domainMax[1], &domainMax[2]);
        } else if (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-' || t[0] == '.') {
            float r, g, b;
            if (std::sscanf(t.c_str(), "%f %f %f", &r, &g, &b) == 3) {
                values.push_back((r - domainMin[0]) / std::max(1e-6f, domainMax[0] - domainMin[0]));
                values.push_back((g - domainMin[1]) / std::max(1e-6f, domainMax[1] - domainMin[1]));
                values.push_back((b - domainMin[2]) / std::max(1e-6f, domainMax[2] - domainMin[2]));
            }
        }
    }
    const int n = lut->size;
    if (n < 2 || n > 128 || values.size() != static_cast<size_t>(n) * n * n * 3) {
        if (error) *error = "Invalid or unsupported .cube file (3D LUT required)";
        return nullptr;
    }
    // .cube order: r fastest, then g, then b. Packed texture: x = r + b*N, y = g.
    lut->rgba.assign(static_cast<size_t>(n) * n * n * 4, 1.0f);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const size_t src = (static_cast<size_t>(b) * n * n + static_cast<size_t>(g) * n + static_cast<size_t>(r)) * 3;
                const size_t dst = (static_cast<size_t>(g) * n * n + static_cast<size_t>(r + b * n)) * 4;
                lut->rgba[dst] = values[src];
                lut->rgba[dst + 1] = values[src + 1];
                lut->rgba[dst + 2] = values[src + 2];
            }
    std::lock_guard lock(m);
    cache[path] = lut;
    return lut;
}

// ============================================================ registry

ParamValue paramValue(const EffectDef& def, const EffectInstance& inst, const std::string& id, Time t) {
    if (const AnimatedParam* p = inst.params.find(id)) return p->evaluate(t);
    for (auto& d : def.params)
        if (d.id == id) return d.def;
    return {};
}

float param1(const EffectDef& def, const EffectInstance& inst, const std::string& id, Time t) {
    return paramValue(def, inst, id, t)[0];
}

EffectRegistry& EffectRegistry::instance() {
    static EffectRegistry r;
    return r;
}

void EffectRegistry::add(EffectDef def) {
    for (auto& d : defs_)
        if (d.id == def.id) {
            d = std::move(def);
            return;
        }
    defs_.push_back(std::move(def));
}

const EffectDef* EffectRegistry::find(const std::string& id) const {
    for (auto& d : defs_)
        if (d.id == id) return &d;
    return nullptr;
}

std::vector<const EffectDef*> EffectRegistry::list(EffectKind kind) const {
    std::vector<const EffectDef*> out;
    for (auto& d : defs_)
        if (d.kind == kind) out.push_back(&d);
    return out;
}

EffectInstance EffectRegistry::instantiate(const std::string& id) const {
    EffectInstance e;
    e.id = newId();
    e.effectId = id;
    if (const EffectDef* d = find(id))
        for (auto& p : d->params) e.params.setStatic(p.id, p.def);
    return e;
}

// ============================================================ video effects

namespace {

#define P1(name) param1(def, inst, name, env.localTime)
#define PV(name) paramValue(def, inst, name, env.localTime)

void registerVideoEffects(EffectRegistry& r) {
    // ---------------- Blur
    r.add({"blur.gaussian", "Gaussian Blur", "Blur", EffectKind::Video,
           {pf("radius", "Radius", 10, 0, 250, "px", 0.5f)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("blur.gaussian");
               return gaussianBlur(env.device, env.pool, in, P1("radius") * env.pixelScale * 0.5f);
           }});

    auto directional = [](const char* id) {
        return [id](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) -> TexturePtr {
            const EffectDef& def = *EffectRegistry::instance().find(id);
            const float len = P1("length") * env.pixelScale;
            if (len < 0.5f) return in;
            const float a = P1("angle") * kDeg;
            TexturePtr out = env.pool.acquire(in->width(), in->height());
            KernelParams p;
            p.set(0, std::cos(a) * len / static_cast<float>(in->width()), std::sin(a) * len / static_cast<float>(in->height()),
                  std::clamp(len, 4.0f, 48.0f), 1.0f);
            env.device.run(Kernel::BlurDirectional, p, {in.get()}, *out);
            return out;
        };
    };
    r.add({"blur.directional", "Directional Blur", "Blur", EffectKind::Video,
           {pf("angle", "Angle", 0, -180, 180, "°", 0.5f, ParamType::Angle), pf("length", "Length", 20, 0, 400, "px", 0.5f)},
           directional("blur.directional")});
    r.add({"blur.motion", "Motion Blur", "Blur", EffectKind::Video,
           {pf("angle", "Angle", 0, -180, 180, "°", 0.5f, ParamType::Angle), pf("length", "Shutter Length", 12, 0, 200, "px", 0.5f)},
           directional("blur.motion"),
           {},
           "Directional shutter blur along the motion angle."});
    r.add({"blur.radial", "Zoom Blur", "Blur", EffectKind::Video,
           {pf("amount", "Amount", 0.15f, 0, 1, "", 0.005f), pvec("center", "Center", 0.5f, 0.5f, 0, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("blur.radial");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const ParamValue c = PV("center");
               KernelParams p;
               p.set(0, c[0], c[1], P1("amount"), 0);
               p.set(1, 32);
               env.device.run(Kernel::BlurRadial, p, {in.get()}, *out);
               return out;
           }});

    // ---------------- Sharpen
    r.add({"sharpen", "Sharpen", "Blur", EffectKind::Video,
           {pf("amount", "Amount", 0.8f, 0, 4), pf("radius", "Radius", 1.5f, 0.3f, 10, "px", 0.1f),
            pf("threshold", "Threshold", 0.0f, 0, 0.2f, "", 0.001f)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("sharpen");
               TexturePtr blurred = gaussianBlur(env.device, env.pool, in, std::max(0.4f, P1("radius") * env.pixelScale));
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, P1("amount"), P1("threshold"));
               env.device.run(Kernel::Sharpen, p, {in.get(), blurred.get()}, *out);
               return out;
           }});

    // ---------------- Stylize: bloom / glow
    r.add({"stylize.bloom", "Bloom", "Stylize", EffectKind::Video,
           {pf("threshold", "Threshold", 0.7f, 0, 1), pf("intensity", "Intensity", 0.8f, 0, 4),
            pf("radius", "Radius", 30, 1, 300, "px", 0.5f)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("stylize.bloom");
               TexturePtr bright = env.pool.acquire(in->width(), in->height());
               KernelParams t;
               t.set(0, P1("threshold"), 0.1f, 0);
               env.device.run(Kernel::Threshold, t, {in.get()}, *bright);
               TexturePtr blurred = gaussianBlur(env.device, env.pool, bright, P1("radius") * env.pixelScale * 0.5f);
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams c;
               c.set(0, 1, P1("intensity"));
               env.device.run(Kernel::Combine, c, {in.get(), blurred.get()}, *out);
               return out;
           }});
    r.add({"stylize.glow", "Glow", "Stylize", EffectKind::Video,
           {pf("threshold", "Threshold", 0.6f, 0, 1), pf("intensity", "Intensity", 1.2f, 0, 5),
            pf("radius", "Radius", 20, 1, 300, "px", 0.5f), pcol("color", "Color", 1.0f, 0.85f, 0.5f),
            pf("tint", "Tint Amount", 0.0f, 0, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("stylize.glow");
               TexturePtr bright = env.pool.acquire(in->width(), in->height());
               KernelParams t;
               t.set(0, P1("threshold"), 0.15f, 0);
               env.device.run(Kernel::Threshold, t, {in.get()}, *bright);
               TexturePtr blurred = gaussianBlur(env.device, env.pool, bright, P1("radius") * env.pixelScale * 0.5f);
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const ParamValue col = PV("color");
               KernelParams c;
               c.set(0, 0, P1("intensity"));
               c.set(1, col[0], col[1], col[2], P1("tint"));
               env.device.run(Kernel::Combine, c, {in.get(), blurred.get()}, *out);
               return out;
           }});

    r.add({"stylize.vignette", "Vignette", "Stylize", EffectKind::Video,
           {pf("amount", "Amount", 0.5f, -1, 1), pf("size", "Size", 0.6f, 0, 2), pf("softness", "Softness", 0.6f, 0, 2),
            pf("roundness", "Roundness", 1.0f, 0, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("stylize.vignette");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, P1("amount"), P1("size"), P1("softness"), P1("roundness"));
               p.set(1, static_cast<float>(in->width()) / static_cast<float>(in->height()));
               env.device.run(Kernel::Vignette, p, {in.get()}, *out);
               return out;
           }});

    auto noise = [](const char* id, bool grain) {
        return [id, grain](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) -> TexturePtr {
            const EffectDef& def = *EffectRegistry::instance().find(id);
            TexturePtr out = env.pool.acquire(in->width(), in->height());
            KernelParams p;
            const double frame = std::floor(env.sequenceSeconds * env.frameRate.toDouble());
            p.set(0, P1("amount"), std::max(0.5f, P1("size") * env.pixelScale), grain ? 1.0f : P1("monochrome"),
                  static_cast<float>(std::fmod(frame * 0.618, 97.0)));
            p.set(1, grain ? 1.0f : 0.0f);
            env.device.run(Kernel::Noise, p, {in.get()}, *out);
            return out;
        };
    };
    r.add({"stylize.noise", "Noise", "Stylize", EffectKind::Video,
           {pf("amount", "Amount", 0.1f, 0, 1), pf("size", "Size", 1.0f, 0.5f, 10, "px"), pbool("monochrome", "Monochrome", false)},
           noise("stylize.noise", false)});
    r.add({"stylize.film_grain", "Film Grain", "Stylize", EffectKind::Video,
           {pf("amount", "Amount", 0.12f, 0, 1), pf("size", "Grain Size", 1.5f, 0.5f, 10, "px")},
           noise("stylize.film_grain", true)});

    r.add({"stylize.pixelate", "Pixelate", "Stylize", EffectKind::Video,
           {pf("cell", "Cell Size", 16, 1, 200, "px", 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("stylize.pixelate");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const float cell = std::max(1.0f, P1("cell") * env.pixelScale);
               KernelParams p;
               p.set(0, cell / static_cast<float>(in->width()), cell / static_cast<float>(in->height()), 0, 0);
               env.device.run(Kernel::Pixelate, p, {in.get()}, *out);
               return out;
           }});
    r.add({"stylize.mosaic", "Mosaic (Region)", "Stylize", EffectKind::Video,
           {pf("cell", "Cell Size", 20, 1, 200, "px", 1), pvec("center", "Center", 0.5f, 0.5f, 0, 1),
            pvec("size", "Size", 0.25f, 0.25f, 0, 1), penum("shape", "Shape", {"Rectangle", "Ellipse"}, 1),
            pf("feather", "Feather", 0.05f, 0, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("stylize.mosaic");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const float cell = std::max(1.0f, P1("cell") * env.pixelScale);
               const ParamValue c = PV("center"), s = PV("size");
               KernelParams p;
               p.set(0, cell / static_cast<float>(in->width()), cell / static_cast<float>(in->height()), 1.0f + P1("shape"),
                     P1("feather"));
               p.set(1, c[0], c[1], s[0] * 0.5f, s[1] * 0.5f);
               env.device.run(Kernel::Pixelate, p, {in.get()}, *out);
               return out;
           }});

    // ---------------- Distort
    r.add({"distort.rgb_split", "RGB Split", "Distort", EffectKind::Video,
           {pf("offset", "Offset", 8, 0, 200, "px", 0.5f), pf("angle", "Angle", 0, -180, 180, "°", 0.5f, ParamType::Angle)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("distort.rgb_split");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const float o = P1("offset") * env.pixelScale, a = P1("angle") * kDeg;
               const float dx = std::cos(a) * o / static_cast<float>(in->width());
               const float dy = std::sin(a) * o / static_cast<float>(in->height());
               KernelParams p;
               p.set(0, dx, dy, -dx, -dy);
               p.set(1, 0, 0);
               env.device.run(Kernel::RgbSplit, p, {in.get()}, *out);
               return out;
           }});
    r.add({"distort.chromatic_aberration", "Chromatic Aberration", "Distort", EffectKind::Video,
           {pf("strength", "Strength", 0.01f, 0, 0.1f, "", 0.0005f)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("distort.chromatic_aberration");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(1, 1.0f, P1("strength"));
               env.device.run(Kernel::RgbSplit, p, {in.get()}, *out);
               return out;
           }});
    r.add({"distort.lens", "Lens Distortion", "Distort", EffectKind::Video,
           {pf("distortion", "Distortion", 0.2f, -1, 1), pf("zoom", "Zoom", 1.0f, 0.5f, 2)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("distort.lens");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const float k = P1("distortion");
               KernelParams p;
               p.set(0, -k, -k * 0.3f, P1("zoom"), 0);
               p.set(1, static_cast<float>(in->width()) / static_cast<float>(in->height()));
               env.device.run(Kernel::LensDistort, p, {in.get()}, *out);
               return out;
           }});
    r.add({"distort.fisheye", "Fisheye", "Distort", EffectKind::Video,
           {pf("strength", "Strength", 2.0f, 0.01f, 8), pf("zoom", "Zoom", 1.0f, 0.3f, 2)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("distort.fisheye");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, 0, 0, P1("zoom"), 1);
               p.set(1, static_cast<float>(in->width()) / static_cast<float>(in->height()), P1("strength"));
               env.device.run(Kernel::LensDistort, p, {in.get()}, *out);
               return out;
           }});
    r.add({"distort.glitch", "Glitch", "Distort", EffectKind::Video,
           {pf("amount", "Amount", 0.5f, 0, 1), pf("speed", "Speed", 12, 0, 60, "Hz", 0.5f),
            pf("block", "Block Size", 24, 2, 200, "px", 1), pf("shift", "Color Shift", 10, 0, 100, "px", 0.5f)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("distort.glitch");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, P1("amount"), static_cast<float>(std::floor(env.sequenceSeconds * P1("speed"))),
                     P1("block") * env.pixelScale / static_cast<float>(in->height()),
                     P1("shift") * env.pixelScale / static_cast<float>(in->width()));
               env.device.run(Kernel::Glitch, p, {in.get()}, *out);
               return out;
           }});
    r.add({"distort.camera_shake", "Camera Shake", "Distort", EffectKind::Video,
           {pf("amplitude", "Amplitude", 12, 0, 200, "px", 0.5f), pf("rotation", "Rotation", 1.0f, 0, 20, "°", 0.1f),
            pf("frequency", "Frequency", 6, 0.1f, 30, "Hz", 0.1f), pf("zoom", "Zoom", 1.06f, 1, 2), pf("seed", "Seed", 1, 0, 100, "", 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("distort.camera_shake");
               const double t = env.sequenceSeconds * P1("frequency");
               const float seed = P1("seed");
               // Smooth pseudo-random motion from summed sines.
               auto wobble = [&](double phase) {
                   return static_cast<float>(0.6 * std::sin(t * 2.1 + phase + seed) + 0.3 * std::sin(t * 3.7 + phase * 2.0 + seed) +
                                             0.1 * std::sin(t * 7.3 + phase * 3.0));
               };
               const float amp = P1("amplitude") * env.pixelScale;
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, wobble(0.0) * amp / static_cast<float>(in->width()), wobble(1.7) * amp / static_cast<float>(in->height()),
                     wobble(3.1) * P1("rotation") * kDeg, P1("zoom"));
               p.set(1, static_cast<float>(in->width()) / static_cast<float>(in->height()));
               env.device.run(Kernel::Shake, p, {in.get()}, *out);
               return out;
           }});

    // ---------------- Color
    r.add({"color.basic", "Color Correction", "Color", EffectKind::Video,
           {pf("exposure", "Exposure", 0, -4, 4, "EV"), pf("contrast", "Contrast", 0, -1, 1), pf("highlights", "Highlights", 0, -1, 1),
            pf("shadows", "Shadows", 0, -1, 1), pf("whites", "Whites", 0, -1, 1), pf("blacks", "Blacks", 0, -1, 1),
            pf("temperature", "Temperature", 0, -1, 1), pf("tint", "Tint", 0, -1, 1), pf("saturation", "Saturation", 1, 0, 2),
            pf("vibrance", "Vibrance", 0, -1, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("color.basic");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, P1("exposure"), P1("contrast"), P1("saturation"), P1("vibrance"));
               p.set(1, P1("highlights"), P1("shadows"), P1("whites"), P1("blacks"));
               p.set(2, P1("temperature"), P1("tint"), 0.435f);
               env.device.run(Kernel::ColorBasic, p, {in.get()}, *out);
               return out;
           }});
    auto wheel = [](const char* id, const char* label) {
        ParamDef d = pcol(id, label, 0, 0, 0, 0);
        d.minValue = -1;
        d.maxValue = 1;
        return d;
    };
    r.add({"color.wheels", "Color Wheels", "Color", EffectKind::Video,
           {wheel("lift", "Lift"), wheel("gamma", "Gamma"), wheel("gain", "Gain"), wheel("offset", "Offset"),
            wheel("shadows", "Shadows"), wheel("midtones", "Midtones"), wheel("highlights", "Highlights")},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("color.wheels");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               int slot = 0;
               for (const char* n : {"lift", "gamma", "gain", "offset", "shadows", "midtones", "highlights"}) {
                   const ParamValue v = PV(n);
                   const float s = (slot == 3 || slot >= 4) ? 0.25f : 0.5f;
                   p.set(slot++, v[0] * s, v[1] * s, v[2] * s, v[3] * s);
               }
               env.device.run(Kernel::ColorWheels, p, {in.get()}, *out);
               return out;
           }});
    r.add({"color.curves", "Curves", "Color", EffectKind::Video,
           {pf("strength", "Strength", 1, 0, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("color.curves");
               static const char* rows[8] = {"master", "red", "green", "blue", "hueHue", "hueSat", "hueLuma", "lumaSat"};
               std::vector<float> lut(256 * 8 * 4, 1.0f);
               for (int r2 = 0; r2 < 8; ++r2) {
                   auto it = inst.properties.find(rows[r2]);
                   const std::string pts = it == inst.properties.end() ? std::string() : it->second;
                   const auto table = buildCurveTable(pts, 256, r2 >= 4);
                   for (int i = 0; i < 256; ++i) lut[(static_cast<size_t>(r2) * 256 + static_cast<size_t>(i)) * 4] = table[static_cast<size_t>(i)];
               }
               TexturePtr lt = env.pool.acquire(256, 8, gpu::TexFormat::RGBA32F);
               env.device.upload(*lt, lut.data(), 256 * 16);
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, P1("strength"));
               env.device.run(Kernel::Curves, p, {in.get(), lt.get()}, *out);
               return out;
           }});
    r.add({"color.lut", "LUT (.cube)", "Color", EffectKind::Video,
           {pf("strength", "Strength", 1, 0, 1)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) -> TexturePtr {
               const EffectDef& def = *EffectRegistry::instance().find("color.lut");
               auto it = inst.properties.find("file");
               if (it == inst.properties.end() || it->second.empty()) return in;
               std::string err;
               auto lut = loadCubeLut(it->second, &err);
               if (!lut) return in;
               const int n = lut->size;
               TexturePtr lt = env.pool.acquire(n * n, n, gpu::TexFormat::RGBA32F);
               env.device.upload(*lt, lut->rgba.data(), n * n * 16);
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(0, static_cast<float>(n), P1("strength"));
               env.device.run(Kernel::Lut3D, p, {in.get(), lt.get()}, *out);
               return out;
           }});

    // ---------------- Keying / masks
    r.add({"key.chroma", "Chroma Key", "Keying", EffectKind::Video,
           {pcol("keyColor", "Key Color", 0.0f, 0.8f, 0.2f), pf("tolerance", "Tolerance", 0.3f, 0, 1),
            pf("softness", "Edge Softness", 0.1f, 0, 1), pf("spill", "Spill Suppression", 0.5f, 0, 1),
            pf("clipBlack", "Matte Clip Black", 0.0f, 0, 1), pf("clipWhite", "Matte Clip White", 1.0f, 0, 1),
            pf("feather", "Feather", 0.0f, 0, 20, "px"), pbool("showMatte", "Show Matte", false)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("key.chroma");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const ParamValue k = PV("keyColor");
               KernelParams p;
               p.set(0, k[0], k[1], k[2], P1("tolerance"));
               p.set(1, P1("softness"), P1("spill"), P1("clipBlack"), P1("clipWhite"));
               p.set(2, P1("showMatte"));
               env.device.run(Kernel::ChromaKey, p, {in.get()}, *out);
               const float feather = P1("feather") * env.pixelScale;
               if (feather > 0.3f && P1("showMatte") < 0.5f) {
                   // Soften the matte edge: blend the keyed result with a blurred copy by alpha edge.
                   TexturePtr soft = gaussianBlur(env.device, env.pool, out, feather);
                   TexturePtr mixed = env.pool.acquire(in->width(), in->height());
                   KernelParams m;
                   m.set(0, 0.5f);
                   env.device.run(Kernel::Mix, m, {out.get(), soft.get()}, *mixed);
                   return mixed;
               }
               return out;
           }});
    r.add({"mask.shape", "Shape Mask", "Mask", EffectKind::Video,
           {penum("shape", "Shape", {"Rectangle", "Ellipse"}, 1), pvec("center", "Center", 0.5f, 0.5f, 0, 1),
            pvec("size", "Size", 0.5f, 0.5f, 0, 2), pf("rotation", "Rotation", 0, -360, 360, "°", 0.5f, ParamType::Angle),
            pf("feather", "Feather", 0.05f, 0, 1), pf("expansion", "Expansion", 0, -0.5f, 0.5f),
            pbool("invert", "Invert", false), pf("opacity", "Opacity", 100, 0, 100, "%", 0.5f, ParamType::Percent)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) {
               const EffectDef& def = *EffectRegistry::instance().find("mask.shape");
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               const ParamValue c = PV("center"), s = PV("size");
               KernelParams p;
               p.set(0, c[0], c[1], s[0] * 0.5f, s[1] * 0.5f);
               p.set(1, P1("rotation") * kDeg, P1("feather"), P1("expansion"), P1("invert"));
               p.set(2, P1("opacity") / 100.0f, P1("shape"), static_cast<float>(in->width()) / static_cast<float>(in->height()));
               env.device.run(Kernel::Mask, p, {in.get()}, *out);
               return out;
           }});
    r.add({"mask.pen", "Pen Mask", "Mask", EffectKind::Video,
           {pf("feather", "Feather", 4, 0, 200, "px", 0.5f), pbool("invert", "Invert", false),
            pf("opacity", "Opacity", 100, 0, 100, "%", 0.5f, ParamType::Percent)},
           [](VideoEnv& env, const EffectInstance& inst, const TexturePtr& in) -> TexturePtr {
               const EffectDef& def = *EffectRegistry::instance().find("mask.pen");
               auto it = inst.properties.find("points");
               if (it == inst.properties.end()) return in;
               std::vector<std::pair<float, float>> poly;
               for (const auto& part : split(it->second, ';')) {
                   const auto xy = split(part, ',');
                   if (xy.size() == 2) poly.emplace_back(std::strtof(xy[0].c_str(), nullptr), std::strtof(xy[1].c_str(), nullptr));
               }
               if (poly.size() < 3) return in;
               // Rasterize (even-odd, 4x vertical supersampling) at reduced resolution.
               const int mw = std::max(8, in->width() / 2), mh = std::max(8, in->height() / 2);
               std::vector<uint8_t> mask(static_cast<size_t>(mw) * static_cast<size_t>(mh), 0);
               for (int y = 0; y < mh; ++y) {
                   int cov[4096];
                   const int cw = std::min(mw, 4096);
                   std::fill(cov, cov + cw, 0);
                   for (int sub = 0; sub < 4; ++sub) {
                       const float fy = (static_cast<float>(y) + (static_cast<float>(sub) + 0.5f) / 4.0f) / static_cast<float>(mh);
                       std::vector<float> xs;
                       for (size_t i = 0; i < poly.size(); ++i) {
                           auto [x0, y0] = poly[i];
                           auto [x1, y1] = poly[(i + 1) % poly.size()];
                           if ((y0 <= fy && y1 > fy) || (y1 <= fy && y0 > fy)) xs.push_back(x0 + (fy - y0) / (y1 - y0) * (x1 - x0));
                       }
                       std::sort(xs.begin(), xs.end());
                       for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                           const int a = std::clamp(static_cast<int>(xs[k] * static_cast<float>(mw) + 0.5f), 0, cw);
                           const int b = std::clamp(static_cast<int>(xs[k + 1] * static_cast<float>(mw) + 0.5f), 0, cw);
                           for (int x = a; x < b; ++x) ++cov[x];
                       }
                   }
                   for (int x = 0; x < cw; ++x) mask[static_cast<size_t>(y) * static_cast<size_t>(mw) + static_cast<size_t>(x)] = static_cast<uint8_t>(cov[x] * 255 / 4);
               }
               TexturePtr mt = env.pool.acquire(mw, mh, gpu::TexFormat::R8);
               env.device.upload(*mt, mask.data(), mw);
               // Convert to RGBA working format so the blur/feather path can be shared.
               TexturePtr mrgba = env.pool.acquire(mw, mh);
               KernelParams cp;
               cp.set(0, 1, 1, 0, 0);
               env.device.run(Kernel::Copy, cp, {mt.get()}, *mrgba);
               const float feather = P1("feather") * env.pixelScale * 0.5f;
               TexturePtr soft = gaussianBlur(env.device, env.pool, mrgba, feather);
               TexturePtr out = env.pool.acquire(in->width(), in->height());
               KernelParams p;
               p.set(1, 0, 0, 0, P1("invert"));
               p.set(2, P1("opacity") / 100.0f, 2.0f, 1.0f);
               env.device.run(Kernel::Mask, p, {in.get(), soft.get()}, *out);
               return out;
           }});
}

#undef P1
#undef PV

}  // namespace

EffectRegistry::EffectRegistry() { registerVideoEffects(*this); }

// ============================================================ transitions

const std::vector<TransitionDef>& transitions() {
    static const std::vector<TransitionDef> defs = [] {
        std::vector<ParamDef> dirParams = {penum("direction", "Direction", {"Left", "Right", "Up", "Down"}, 0),
                                           pf("softness", "Softness", 0.05f, 0, 1)};
        std::vector<TransitionDef> v = {
            {"cross-dissolve", "Cross Dissolve", "Dissolve", 0, pv(0, 0, 0, 1), {}, 1.0},
            {"fade", "Fade", "Dissolve", 0, pv(0, 0, 0, 1), {}, 1.0},
            {"additive-dissolve", "Additive Dissolve", "Dissolve", 12, pv(0, 0, 0, 1), {}, 1.0},
            {"dip-black", "Dip to Black", "Dissolve", 1, pv(0, 0, 0, 1), {}, 1.0},
            {"dip-white", "Dip to White", "Dissolve", 1, pv(1, 1, 1, 1), {}, 1.0},
            {"slide", "Slide", "Slide", 2, pv(0, 0, 0, 1), dirParams, 0.6},
            {"push", "Push", "Slide", 3, pv(0, 0, 0, 1), dirParams, 0.6},
            {"whip", "Whip Pan", "Slide", 9, pv(0, 0, 0, 1), dirParams, 0.4},
            {"zoom", "Zoom", "Zoom", 4, pv(0, 0, 0, 1), {}, 0.6},
            {"blur", "Blur", "Blur", 5, pv(0, 0, 0, 1), {}, 0.8},
            {"spin", "Spin", "Zoom", 6, pv(0, 0, 0, 1), {}, 0.6},
            {"flash", "Flash", "Light", 7, pv(1, 1, 1, 1), {}, 0.5},
            {"glitch", "Glitch", "Digital", 8, pv(0, 0, 0, 1), {}, 0.5},
            {"wipe", "Wipe", "Wipe", 10, pv(0, 0, 0, 1), dirParams, 0.8},
            {"circle-wipe", "Circle Wipe", "Wipe", 11, pv(0, 0, 0, 1), {pf("softness", "Softness", 0.05f, 0, 1)}, 0.8},
        };
        return v;
    }();
    return defs;
}

const TransitionDef* findTransition(const std::string& id) {
    for (auto& t : transitions())
        if (t.id == id) return &t;
    return nullptr;
}

TexturePtr applyTransition(gpu::Device& dev, gpu::TexturePool& pool, const TransitionSpec& spec, float progress,
                           const TexturePtr& a, const TexturePtr& b, int width, int height) {
    const TransitionDef* def = findTransition(spec.type);
    if (!def) def = &transitions().front();
    TexturePtr out = pool.acquire(width, height);
    KernelParams p;
    const float dir = spec.params.evaluate1("direction", Time{0}, 0.0f);
    const float soft = spec.params.evaluate1("softness", Time{0}, 0.05f);
    p.set(0, static_cast<float>(def->kernelType), progress, dir, soft);
    p.set(1, def->color[0], def->color[1], def->color[2], static_cast<float>(width) / static_cast<float>(height));
    dev.run(Kernel::Transition, p, {a.get(), b.get()}, *out);
    return out;
}

}  // namespace avc::fx
