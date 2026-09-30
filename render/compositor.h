#pragma once
// Compositor: renders one frame of a sequence (preview or export) on a
// gpu::Device. Pipeline per layer (bottom -> top):
//   source (decode+upload / text / solid / nested sequence)
//   -> clip effects (layer space) -> transform/crop/opacity/blend composite.
// Transitions render both sides full-frame and mix them; adjustment clips
// apply their effects to everything below.

#include <memory>

#include "core/jobs.h"
#include "core/result.h"
#include "effects/effects.h"
#include "render/frame_provider.h"
#include "render/gpu/gpu.h"
#include "text/text_raster.h"
#include "timeline/evaluate.h"

namespace avc {

struct RenderOptions {
    int width = 0;   // output size in pixels (0 = sequence size)
    int height = 0;
    bool background = true;  // opaque sequence background colour
    bool forExport = false;
    int maxNesting = 8;
};

struct RenderStats {
    double milliseconds = 0;
    int layers = 0;
    int missingFrames = 0;
    std::string lastError;
};

// 2x3 affine transform (row-major): x' = a*x + b*y + c, y' = d*x + e*y + f.
struct Affine {
    float a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
    static Affine translate(float x, float y) { return {1, 0, x, 0, 1, y}; }
    static Affine scale(float x, float y) { return {x, 0, 0, 0, y, 0}; }
    static Affine rotate(float radians);
    Affine operator*(const Affine& o) const;  // this after o
    [[nodiscard]] Affine inverse() const;
    void apply(float& x, float& y) const {
        const float nx = a * x + b * y + c;
        const float ny = d * x + e * y + f;
        x = nx;
        y = ny;
    }
};

// Layer placement in sequence pixels, shared by compositor, viewer overlays
// (transform handles) and tests.
struct LayerGeometry {
    float width = 0, height = 0;  // layer logical size (texture space units)
    Affine toSequence;            // layer px -> sequence px
    float cropX0 = 0, cropY0 = 0, cropX1 = 0, cropY1 = 0;  // in layer px
};
LayerGeometry computeLayerGeometry(const Clip& clip, Time local, float layerW, float layerH, int rotation,
                                   Rational sampleAspect, int seqW, int seqH);

class Compositor {
public:
    Compositor(gpu::Device& device, IFrameProvider& frames, std::shared_ptr<text::ITextRasterizer> text);

    // Renders `seq` at `t`. Result: premultiplied RGBA16F texture.
    Result<gpu::TexturePtr> render(const ProjectPtr& project, const Sequence& seq, Time t, const RenderOptions& opt,
                                   const CancelToken& cancel = {});

    gpu::TexturePool& pool() { return pool_; }
    gpu::Device& device() { return device_; }
    [[nodiscard]] const RenderStats& stats() const { return stats_; }

private:
    struct LayerImage {
        gpu::TexturePtr tex;
        float width = 0, height = 0;  // logical size
        int rotation = 0;
        Rational sar{1, 1};
        float pixelScale = 1;
    };
    Result<gpu::TexturePtr> renderSequence(const ProjectPtr& project, const Sequence& seq, Time t, int w, int h,
                                           bool background, int depth, const CancelToken& cancel);
    Result<LayerImage> sourceImage(const ProjectPtr& project, const Sequence& seq, const ClipSample& s, float renderScale,
                                   int depth, const CancelToken& cancel);
    gpu::TexturePtr textImage(const Clip& clip, Time local, const Sequence& seq, float renderScale, LayerImage& out);
    gpu::TexturePtr applyEffects(const std::vector<EffectInstance>& effects, gpu::TexturePtr tex, Time local,
                                 const Sequence& seq, Time seqTime, float pixelScale);
    gpu::TexturePtr compositeLayer(const gpu::TexturePtr& accum, const LayerImage& img, const Clip& clip, Time local,
                                   const Sequence& seq, int w, int h, float opacityMul, bool overAccum);
    gpu::TexturePtr fullFrame(const ProjectPtr& project, const Sequence& seq, const ClipSample& s, Time seqTime, int w,
                              int h, int depth, const CancelToken& cancel);

    gpu::Device& device_;
    IFrameProvider& frames_;
    std::shared_ptr<text::ITextRasterizer> text_;
    gpu::TexturePool pool_;
    RenderStats stats_;
};

}  // namespace avc
