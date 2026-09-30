#include "render/compositor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/log.h"
#include "core/strings.h"

namespace avc {

using gpu::Kernel;
using gpu::KernelParams;
using gpu::TexturePtr;

namespace {
constexpr float kDegToRad = 3.14159265358979f / 180.0f;
}

// ---------------------------------------------------------------- Affine

Affine Affine::rotate(float r) {
    const float c = std::cos(r), s = std::sin(r);
    return {c, -s, 0, s, c, 0};
}

Affine Affine::operator*(const Affine& o) const {
    // (this * o)(p) = this(o(p))
    return {a * o.a + b * o.d, a * o.b + b * o.e, a * o.c + b * o.f + c,
            d * o.a + e * o.d, d * o.b + e * o.e, d * o.c + e * o.f + f};
}

Affine Affine::inverse() const {
    const float det = a * e - b * d;
    if (std::fabs(det) < 1e-12f) return {0, 0, -1e9f, 0, 0, -1e9f};
    const float id = 1.0f / det;
    const float na = e * id, nb = -b * id, nd = -d * id, ne = a * id;
    return {na, nb, -(na * c + nb * f), nd, ne, -(nd * c + ne * f)};
}

// ---------------------------------------------------------------- geometry

LayerGeometry computeLayerGeometry(const Clip& clip, Time local, float lw, float lh, int rotation, Rational sar,
                                   int seqW, int seqH) {
    LayerGeometry g;
    g.width = lw;
    g.height = lh;
    const float sarX = sar.num > 0 && sar.den > 0 ? static_cast<float>(sar.toDouble()) : 1.0f;
    // Texture space -> display space (pixel aspect + rotation metadata).
    const float sw = lw * sarX, sh = lh;
    const bool swap = rotation == 90 || rotation == 270;
    const float dispW = swap ? sh : sw, dispH = swap ? sw : sh;
    const Affine toDisplay = Affine::translate(dispW * 0.5f, dispH * 0.5f) * Affine::rotate(static_cast<float>(rotation) * kDegToRad) *
                             Affine::translate(-sw * 0.5f, -sh * 0.5f) * Affine::scale(sarX, 1.0f);

    const ParamSet& tp = clip.transform;
    int fit = static_cast<int>(tp.evaluate1("fitMode", local, 0.0f));
    if (clip.kind == ClipKind::Text || clip.kind == ClipKind::Subtitle) fit = 3;
    if (clip.kind == ClipKind::Solid || clip.kind == ClipKind::Adjustment) fit = 2;
    float bx = 1.0f, by = 1.0f;
    const float fw = static_cast<float>(seqW) / std::max(1.0f, dispW), fh = static_cast<float>(seqH) / std::max(1.0f, dispH);
    switch (fit) {
    case 0: bx = by = std::min(fw, fh); break;
    case 1: bx = by = std::max(fw, fh); break;
    case 2: bx = fw; by = fh; break;
    default: break;
    }
    const ParamValue scale = tp.evaluate("scale", local, pv(100, 100));
    const ParamValue pos = tp.evaluate("position", local, pv(0, 0));
    const ParamValue anchor = tp.evaluate("anchor", local, pv(0, 0));
    const float rot = tp.evaluate1("rotation", local, 0.0f) * kDegToRad;
    const float sx = bx * scale[0] / 100.0f * (clip.flipH ? -1.0f : 1.0f);
    const float sy = by * scale[1] / 100.0f * (clip.flipV ? -1.0f : 1.0f);
    float baseY = 0.0f;
    if (clip.kind == ClipKind::Subtitle) {
        // Subtitles sit near the bottom (6% margin) by default.
        baseY = static_cast<float>(seqH) * 0.5f - dispH * std::fabs(sy) * 0.5f - static_cast<float>(seqH) * 0.06f;
    }
    const Affine user = Affine::translate(static_cast<float>(seqW) * 0.5f + pos[0], static_cast<float>(seqH) * 0.5f + pos[1] + baseY) *
                        Affine::rotate(rot) * Affine::scale(sx, sy) *
                        Affine::translate(-(dispW * 0.5f + anchor[0]), -(dispH * 0.5f + anchor[1]));
    g.toSequence = user * toDisplay;

    // Crop (percent of the displayed layer) mapped back to texture space.
    const float cl = std::clamp(tp.evaluate1("cropLeft", local, 0.0f), 0.0f, 100.0f) / 100.0f;
    const float cr = std::clamp(tp.evaluate1("cropRight", local, 0.0f), 0.0f, 100.0f) / 100.0f;
    const float ct = std::clamp(tp.evaluate1("cropTop", local, 0.0f), 0.0f, 100.0f) / 100.0f;
    const float cb = std::clamp(tp.evaluate1("cropBottom", local, 0.0f), 0.0f, 100.0f) / 100.0f;
    float xs[2] = {dispW * cl, dispW * (1.0f - cr)};
    float ys[2] = {dispH * ct, dispH * (1.0f - cb)};
    const Affine back = toDisplay.inverse();
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (float x : xs)
        for (float y : ys) {
            float px = x, py = y;
            back.apply(px, py);
            minX = std::min(minX, px);
            maxX = std::max(maxX, px);
            minY = std::min(minY, py);
            maxY = std::max(maxY, py);
        }
    g.cropX0 = std::max(0.0f, minX);
    g.cropY0 = std::max(0.0f, minY);
    g.cropX1 = std::min(lw, maxX);
    g.cropY1 = std::min(lh, maxY);
    return g;
}

// ---------------------------------------------------------------- Compositor

Compositor::Compositor(gpu::Device& device, IFrameProvider& frames, std::shared_ptr<text::ITextRasterizer> text)
    : device_(device), frames_(frames), text_(std::move(text)), pool_(device) {}

Result<TexturePtr> Compositor::render(const ProjectPtr& project, const Sequence& seq, Time t, const RenderOptions& opt,
                                      const CancelToken& cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    stats_ = {};
    const int w = opt.width > 0 ? opt.width : seq.width;
    const int h = opt.height > 0 ? opt.height : seq.height;
    auto r = renderSequence(project, seq, t, w, h, opt.background, 0, cancel);
    stats_.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

TexturePtr Compositor::applyEffects(const std::vector<EffectInstance>& effects, TexturePtr tex, Time local,
                                    const Sequence& seq, Time seqTime, float pixelScale) {
    auto& reg = fx::EffectRegistry::instance();
    for (const auto& inst : effects) {
        if (!inst.enabled) continue;
        const fx::EffectDef* def = reg.find(inst.effectId);
        if (!def || def->kind != fx::EffectKind::Video || !def->apply) continue;
        fx::VideoEnv env{device_, pool_, pixelScale, local, seqTime.seconds(), seq.frameRate, seq.width, seq.height};
        TexturePtr out = def->apply(env, inst, tex);
        if (out) tex = std::move(out);
    }
    return tex;
}

TexturePtr Compositor::textImage(const Clip& clip, Time local, const Sequence& seq, float renderScale, LayerImage& out) {
    const ParamSet& p = clip.textParams;
    const ParamValue scale = clip.transform.evaluate("scale", local, pv(100, 100));
    const float userScale = std::max(std::fabs(scale[0]), std::fabs(scale[1])) / 100.0f;
    const float rasterScale = std::clamp(renderScale * userScale, 0.05f, 4.0f);
    const float strokeW = p.evaluate1("strokeWidth", local, 0.0f);
    const ParamValue shadowCol = p.evaluate("shadowColor", local, pv(0, 0, 0, 0));
    const ParamValue shadowOff = p.evaluate("shadowOffset", local, pv(4, 4));
    const float shadowBlur = p.evaluate1("shadowBlur", local, 0.0f);
    const ParamValue glowCol = p.evaluate("glowColor", local, pv(1, 1, 1, 0));
    const float glowR = p.evaluate1("glowRadius", local, 0.0f);
    const ParamValue bgCol = p.evaluate("backgroundColor", local, pv(0, 0, 0, 0));
    const float bgPad = p.evaluate1("backgroundPadding", local, 16.0f);
    const float reveal = std::clamp(p.evaluate1("reveal", local, 100.0f), 0.0f, 100.0f);
    const float boxWidth = p.evaluate1("boxWidth", local, 0.0f);

    text::TextRequest req;
    req.text = clip.text;
    req.style = clip.textStyle;
    req.scale = rasterScale;
    req.wrapWidth = boxWidth > 0 ? boxWidth : (clip.kind == ClipKind::Subtitle ? static_cast<float>(seq.width) * 0.9f : 0.0f);
    if (reveal < 99.999f) {
        const size_t chars = utf8Length(clip.text);
        req.revealChars = static_cast<int>(std::floor(static_cast<double>(chars) * reveal / 100.0));
    }
    req.strokeWidth = strokeW;
    const bool shadowOn = shadowCol[3] > 0.001f;
    const bool glowOn = glowCol[3] > 0.001f && glowR > 0.0f;
    float pad = 2.0f;
    if (shadowOn) pad = std::max(pad, shadowBlur * 1.5f + std::max(std::fabs(shadowOff[0]), std::fabs(shadowOff[1])));
    if (glowOn) pad = std::max(pad, glowR * 1.5f);
    if (bgCol[3] > 0.001f) pad = std::max(pad, bgPad + 1.0f);
    req.padding = pad;
    auto raster = text_ ? text_->rasterize(req) : Result<text::TextRasterPtr>::error("no text rasterizer");
    if (!raster) {
        stats_.lastError = raster.errorMessage();
        return nullptr;
    }
    const text::TextRaster& tr = **raster;
    const int W = tr.width, H = tr.height;
    auto toRgba = [&](const std::vector<uint8_t>& m) {
        std::vector<uint8_t> rgba(static_cast<size_t>(W) * static_cast<size_t>(H) * 4);
        for (size_t i = 0; i < m.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = m[i];
        return rgba;
    };
    TexturePtr fill = pool_.acquire(W, H, gpu::TexFormat::RGBA8);
    device_.upload(*fill, toRgba(tr.fill).data(), W * 4);
    TexturePtr stroke;
    const float strokePx = strokeW * rasterScale;
    if (strokePx > 0.01f) {
        if (!tr.stroke.empty()) {
            stroke = pool_.acquire(W, H, gpu::TexFormat::RGBA8);
            device_.upload(*stroke, toRgba(tr.stroke).data(), W * 4);
        } else {
            stroke = pool_.acquire(W, H);
            KernelParams dp;
            dp.set(0, std::min(strokePx, 40.0f), 1.0f / static_cast<float>(W), 1.0f / static_cast<float>(H));
            device_.run(Kernel::Dilate, dp, {fill.get()}, *stroke);
        }
    }
    const TexturePtr& outline = stroke ? stroke : fill;
    TexturePtr shadow, glow;
    if (shadowOn) shadow = fx::gaussianBlur(device_, pool_, outline, shadowBlur * rasterScale * 0.5f);
    if (glowOn) glow = fx::gaussianBlur(device_, pool_, outline, glowR * rasterScale * 0.5f);

    const ParamValue fillCol = p.evaluate("fillColor", local, pv(1, 1, 1, 1));
    const ParamValue gradCol = p.evaluate("gradientColor", local, pv(1, 1, 1, 1));
    const ParamValue strokeCol = p.evaluate("strokeColor", local, pv(0, 0, 0, 1));
    KernelParams k;
    k.set(0, fillCol[0], fillCol[1], fillCol[2], fillCol[3]);
    k.set(1, gradCol[0], gradCol[1], gradCol[2], gradCol[3]);
    k.set(2, p.evaluate1("gradientEnabled", local, 0.0f), tr.boxY / static_cast<float>(H), (tr.boxY + tr.boxH) / static_cast<float>(H));
    k.set(3, strokeCol[0], strokeCol[1], strokeCol[2], strokeCol[3]);
    k.set(4, shadowCol[0], shadowCol[1], shadowCol[2], shadowCol[3]);
    k.set(5, shadowOff[0] * rasterScale / static_cast<float>(W), shadowOff[1] * rasterScale / static_cast<float>(H),
          stroke ? 1.0f : 0.0f, shadowOn ? 1.0f : 0.0f);
    k.set(6, glowCol[0], glowCol[1], glowCol[2], glowOn ? glowCol[3] : 0.0f);
    k.set(7, bgCol[0], bgCol[1], bgCol[2], bgCol[3]);
    const float padPx = bgPad * rasterScale;
    k.set(8, (tr.boxX - padPx) / static_cast<float>(W), (tr.boxY - padPx) / static_cast<float>(H),
          (tr.boxX + tr.boxW + padPx) / static_cast<float>(W), (tr.boxY + tr.boxH + padPx) / static_cast<float>(H));
    TexturePtr outTex = pool_.acquire(W, H);
    device_.run(Kernel::TextCompose, k, {fill.get(), stroke.get(), shadow.get(), glow.get()}, *outTex);
    out.width = static_cast<float>(W) / rasterScale;
    out.height = static_cast<float>(H) / rasterScale;
    out.pixelScale = rasterScale;
    return outTex;
}

Result<Compositor::LayerImage> Compositor::sourceImage(const ProjectPtr& project, const Sequence& seq, const ClipSample& s,
                                                       float renderScale, int depth, const CancelToken& cancel) {
    const Clip& clip = *s.clip;
    LayerImage img;
    switch (clip.kind) {
    case ClipKind::Media: {
        const MediaItem* media = project->findMedia(clip.media);
        if (!media) return Result<LayerImage>::error("Media missing");
        auto frame = frames_.videoFrame(*media, clip.streamIndex, s.sourceTime, clip.id, cancel);
        if (!frame) {
            ++stats_.missingFrames;
            // "Media offline" placeholder keeps the layout visible.
            img.tex = pool_.acquire(16, 9);
            KernelParams p;
            p.set(0, 0.45f, 0.05f, 0.08f, 1.0f);
            device_.run(Kernel::Fill, p, {}, *img.tex);
            const VideoStreamInfo* v = media->info.primaryVideo();
            img.width = v && v->width > 0 ? static_cast<float>(v->width) : static_cast<float>(seq.width);
            img.height = v && v->height > 0 ? static_cast<float>(v->height) : static_cast<float>(seq.height);
            stats_.lastError = frame.errorMessage();
            return img;
        }
        const VideoFrame& f = **frame;
        img.tex = gpu::uploadVideoFrame(device_, pool_, f);
        if (!img.tex) return Result<LayerImage>::error("Frame upload failed");
        // Logical size comes from the original media so that proxies (smaller
        // frames) produce exactly the same geometry as the full-resolution file.
        const VideoStreamInfo* vinfo = media->info.primaryVideo();
        const bool known = vinfo && vinfo->width > 0 && vinfo->height > 0;
        img.width = known ? static_cast<float>(vinfo->width) : static_cast<float>(f.width);
        img.height = known ? static_cast<float>(vinfo->height) : static_cast<float>(f.height);
        img.rotation = f.rotation;
        img.sar = known && vinfo->sampleAspect.num > 0 ? vinfo->sampleAspect : f.sampleAspect;
        img.pixelScale = img.width > 0 ? static_cast<float>(f.width) / img.width : 1.0f;
        return img;
    }
    case ClipKind::Text:
    case ClipKind::Subtitle: {
        img.tex = textImage(clip, s.localTime, seq, renderScale, img);
        if (!img.tex) return Result<LayerImage>::error("Text rendering failed: " + stats_.lastError);
        return img;
    }
    case ClipKind::Solid: {
        img.tex = pool_.acquire(8, 8);
        const ParamValue c = clip.solidColor;
        KernelParams p;
        p.set(0, c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]);
        device_.run(Kernel::Fill, p, {}, *img.tex);
        img.width = static_cast<float>(seq.width);
        img.height = static_cast<float>(seq.height);
        return img;
    }
    case ClipKind::Compound: {
        const Sequence* nested = project->findSequence(clip.nested);
        if (!nested) return Result<LayerImage>::error("Nested sequence missing");
        if (depth >= 8 || nested->id == seq.id) return Result<LayerImage>::error("Nesting too deep");
        const int nw = std::max(2, static_cast<int>(std::lround(nested->width * renderScale)));
        const int nh = std::max(2, static_cast<int>(std::lround(nested->height * renderScale)));
        auto tex = renderSequence(project, *nested, s.sourceTime, nw, nh, false, depth + 1, cancel);
        if (!tex) return tex.status();
        img.tex = *tex;
        img.width = static_cast<float>(nested->width);
        img.height = static_cast<float>(nested->height);
        img.pixelScale = renderScale;
        return img;
    }
    case ClipKind::Adjustment: break;
    }
    return Result<LayerImage>::error("Unsupported clip kind");
}

TexturePtr Compositor::compositeLayer(const TexturePtr& accum, const LayerImage& img, const Clip& clip, Time local,
                                      const Sequence& seq, int w, int h, float opacityMul, bool overAccum) {
    const LayerGeometry g = computeLayerGeometry(clip, local, img.width, img.height, img.rotation, img.sar, seq.width, seq.height);
    const float rx = static_cast<float>(w) / static_cast<float>(seq.width);
    const float ry = static_cast<float>(h) / static_cast<float>(seq.height);
    const Affine toOut = Affine::scale(rx, ry) * g.toSequence;
    const Affine inv = toOut.inverse();
    const float pxPerLayer = std::sqrt(std::fabs(toOut.a * toOut.e - toOut.b * toOut.d));
    const float opacity = std::clamp(clip.transform.evaluate1("opacity", local, 100.0f) / 100.0f, 0.0f, 1.0f) * opacityMul;
    TexturePtr out = pool_.acquire(w, h);
    KernelParams p;
    // Kernel samples at pixel centres; inverse maps output px -> layer px.
    p.set(0, inv.a, inv.b, inv.c);
    p.set(1, inv.d, inv.e, inv.f);
    p.set(2, img.width, img.height, 1.0f / std::max(1e-6f, img.width), 1.0f / std::max(1e-6f, img.height));
    p.set(3, g.cropX0, g.cropY0, g.cropX1, g.cropY1);
    p.set(4, opacity, static_cast<float>(clip.blend), std::max(pxPerLayer, 0.001f), overAccum && accum ? 1.0f : 0.0f);
    device_.run(Kernel::Composite, p, {accum.get(), img.tex.get()}, *out);
    return out;
}

TexturePtr Compositor::fullFrame(const ProjectPtr& project, const Sequence& seq, const ClipSample& s, Time seqTime, int w,
                                 int h, int depth, const CancelToken& cancel) {
    if (!s.clip) {
        TexturePtr empty = pool_.acquire(w, h);
        device_.clear(*empty, 0, 0, 0, 0);
        return empty;
    }
    const float renderScale = static_cast<float>(w) / static_cast<float>(seq.width);
    auto img = sourceImage(project, seq, s, renderScale, depth, cancel);
    if (!img) {
        TexturePtr empty = pool_.acquire(w, h);
        device_.clear(*empty, 0, 0, 0, 0);
        return empty;
    }
    img->tex = applyEffects(s.clip->effects, img->tex, s.localTime, seq, seqTime, img->pixelScale);
    return compositeLayer(nullptr, *img, *s.clip, s.localTime, seq, w, h, 1.0f, false);
}

Result<TexturePtr> Compositor::renderSequence(const ProjectPtr& project, const Sequence& seq, Time t, int w, int h,
                                              bool background, int depth, const CancelToken& cancel) {
    TexturePtr accum = pool_.acquire(w, h);
    if (!accum) return Result<TexturePtr>::error("Out of GPU memory");
    if (background) {
        const ParamValue bg = seq.backgroundColor;
        device_.clear(*accum, bg[0], bg[1], bg[2], 1.0f);
    } else {
        device_.clear(*accum, 0, 0, 0, 0);
    }
    const float renderScale = static_cast<float>(w) / static_cast<float>(seq.width);
    const FramePlan plan = planFrame(project, seq, t);
    for (const VisualLayer& layer : plan.layers) {
        if (cancel.cancelled()) return Result<TexturePtr>::error("cancelled");
        ++stats_.layers;
        if (layer.isTransition()) {
            TexturePtr a = fullFrame(project, seq, layer.a, t, w, h, depth, cancel);
            TexturePtr b = fullFrame(project, seq, layer.b, t, w, h, depth, cancel);
            TexturePtr mixed = fx::applyTransition(device_, pool_, *layer.transition, layer.progress, a, b, w, h);
            LayerImage img;
            img.tex = mixed;
            img.width = static_cast<float>(seq.width);
            img.height = static_cast<float>(seq.height);
            Clip identity;
            identity.transform.setStatic("fitMode", pv(2));  // stretch = exact frame
            identity.kind = ClipKind::Solid;
            accum = compositeLayer(accum, img, identity, Time{0}, seq, w, h, 1.0f, true);
            continue;
        }
        const Clip& clip = *layer.a.clip;
        if (clip.kind == ClipKind::Adjustment) {
            if (clip.effects.empty()) continue;
            TexturePtr fxTex = applyEffects(clip.effects, accum, layer.a.localTime, seq, t, renderScale);
            const float opacity = std::clamp(clip.transform.evaluate1("opacity", layer.a.localTime, 100.0f) / 100.0f, 0.0f, 1.0f);
            if (opacity >= 0.999f) {
                accum = fxTex;
            } else {
                TexturePtr mixed = pool_.acquire(w, h);
                KernelParams p;
                p.set(0, opacity);
                device_.run(Kernel::Mix, p, {accum.get(), fxTex.get()}, *mixed);
                accum = mixed;
            }
            continue;
        }
        auto img = sourceImage(project, seq, layer.a, renderScale, depth, cancel);
        if (!img) {
            stats_.lastError = img.errorMessage();
            continue;
        }
        img->tex = applyEffects(clip.effects, img->tex, layer.a.localTime, seq, t, img->pixelScale);
        accum = compositeLayer(accum, *img, clip, layer.a.localTime, seq, w, h, 1.0f, true);
    }
    return accum;
}

}  // namespace avc
