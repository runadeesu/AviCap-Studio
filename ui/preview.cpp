#include "ui/preview.h"

#include <algorithm>
#include <chrono>

#include "core/log.h"
#include "core/platform.h"
#include "effects/effects.h"

namespace avc::ui {

const char* previewQualityName(PreviewQuality q) {
    switch (q) {
    case PreviewQuality::Full: return "full";
    case PreviewQuality::Half: return "half";
    case PreviewQuality::Quarter: return "quarter";
    case PreviewQuality::Eighth: return "eighth";
    default: return "auto";
    }
}

PreviewQuality previewQualityFromName(const std::string& s) {
    if (s == "full") return PreviewQuality::Full;
    if (s == "half") return PreviewQuality::Half;
    if (s == "quarter") return PreviewQuality::Quarter;
    if (s == "eighth") return PreviewQuality::Eighth;
    return PreviewQuality::Auto;
}

PreviewRenderer::PreviewRenderer(gpu::Device& device, std::shared_ptr<text::ITextRasterizer> text, FrameProviderOptions frames)
    : device_(device),
      frames_(std::make_unique<MediaFrameProvider>(frames)),
      compositor_(std::make_unique<Compositor>(device, *frames_, std::move(text))) {
    thread_ = std::thread([this] { loop(); });
}

PreviewRenderer::~PreviewRenderer() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PreviewRenderer::request(PreviewRequest r) {
    {
        std::lock_guard lk(mutex_);
        if (hasPending_ && pendingSerial_ > renderedSerial_ && pending_.playing) ++stats_.superseded;
        pending_ = std::move(r);
        ++pendingSerial_;
        hasPending_ = true;
    }
    cv_.notify_one();
}

PreviewFrame PreviewRenderer::latest() const {
    std::lock_guard lk(mutex_);
    return latest_;
}

PreviewStats PreviewRenderer::stats() const {
    std::lock_guard lk(mutex_);
    PreviewStats s = stats_;
    s.openDecoders = frames_->openDecoderCount();
    s.decoder = frames_->lastDecoderName();
    return s;
}

bool PreviewRenderer::waitIdle(int timeoutMs) {
    std::unique_lock lk(mutex_);
    return idleCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return stop_ || renderedSerial_ >= pendingSerial_; });
}

void PreviewRenderer::invalidate() {
    {
        std::lock_guard lk(mutex_);
        force_ = true;
        if (hasPending_) ++pendingSerial_;
    }
    cv_.notify_one();
}

void PreviewRenderer::setOnFrame(std::function<void()> fn) {
    std::lock_guard lk(mutex_);
    onFrame_ = std::move(fn);
}

void PreviewRenderer::trimMemory() {
    compositor_->pool().trim();
    frames_->cache().clear();
}

int PreviewRenderer::chooseDivisor(const PreviewRequest& r, const Sequence& seq) {
    switch (r.quality) {
    case PreviewQuality::Full: return 1;
    case PreviewQuality::Half: return 2;
    case PreviewQuality::Quarter: return 4;
    case PreviewQuality::Eighth: return 8;
    default: break;
    }
    (void)seq;
    return r.playing ? autoDivisor_ : 1;
}

void PreviewRenderer::loop() {
    setCurrentThreadName("AviCap Preview");
    for (;;) {
        PreviewRequest req;
        uint64_t serial = 0;
        bool force = false;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || (hasPending_ && pendingSerial_ > renderedSerial_); });
            if (stop_) break;
            req = pending_;
            serial = pendingSerial_;
            force = force_;
            force_ = false;
        }
        auto finish = [&](PreviewFrame* frame) {
            std::function<void()> cb;
            {
                std::lock_guard lk(mutex_);
                if (frame) latest_ = std::move(*frame);
                renderedSerial_ = std::max(renderedSerial_, serial);
                cb = onFrame_;
            }
            idleCv_.notify_all();
            if (cb && frame) cb();
        };

        const Sequence* seq = req.project ? req.project->findSequence(req.sequence) : nullptr;
        if (!seq) {
            PreviewFrame empty;
            empty.error = req.project ? "Sequence not found" : "";
            finish(&empty);
            continue;
        }
        const int div = chooseDivisor(req, *seq);
        int w = std::max(16, seq->width / div);
        int h = std::max(16, seq->height / div);
        if (req.viewportWidth > 0 && req.viewportHeight > 0 && req.quality == PreviewQuality::Auto) {
            // No point rendering more pixels than the viewer shows.
            const double fit = std::min(double(req.viewportWidth) / seq->width, double(req.viewportHeight) / seq->height);
            if (fit * seq->width < w) {
                w = std::max(16, static_cast<int>(seq->width * fit + 0.5));
                h = std::max(16, static_cast<int>(seq->height * fit + 0.5));
            }
        }
        w &= ~1;
        h &= ~1;

        bool same = false;
        {
            std::lock_guard lk(mutex_);
            same = !force && latest_.texture && latest_.project == req.project.get() && latest_.sequence == req.sequence &&
                   latest_.time == req.time && latest_.width == w && latest_.height == h &&
                   (!req.wantScopes || latest_.scopes);
        }
        if (same) {
            finish(nullptr);
            continue;
        }

        const auto t0 = std::chrono::steady_clock::now();
        RenderOptions opt;
        opt.width = w;
        opt.height = h;
        PreviewFrame f;
        f.time = req.time;
        f.project = req.project.get();
        f.sequence = req.sequence;
        f.width = w;
        f.height = h;
        f.divisor = div;
        f.serial = serial;
        try {
            auto r = compositor_->render(req.project, *seq, req.time, opt);
            if (r) {
                f.texture = *r;
                f.missingFrames = compositor_->stats().missingFrames;
                if (req.wantScopes) {
                    // Progressive halving keeps the scope image alias-free.
                    gpu::TexturePtr small = f.texture;
                    const int target = 320;
                    while (small->width() / 2 >= target)
                        small = fx::resample(device_, compositor_->pool(), small, small->width() / 2, std::max(1, small->height() / 2));
                    if (small->width() > target)
                        small = fx::resample(device_, compositor_->pool(), small, target,
                                             std::max(1, small->height() * target / small->width()));
                    auto img = std::make_shared<cache::Image>();
                    img->width = small->width();
                    img->height = small->height();
                    img->rgba = gpu::readbackRgba8(device_, *small, true);
                    f.scopes = std::move(img);
                }
                device_.flush();
            } else {
                f.error = r.errorMessage();
            }
        } catch (const std::exception& e) {
            f.error = e.what();
            AVC_ERROR("preview", "render failed: {}", e.what());
        }
        if (device_.deviceLost()) f.error = "GPU device lost";
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        f.renderMs = ms;

        // Adaptive resolution while playing.
        if (req.playing && req.quality == PreviewQuality::Auto) {
            const double budget = 1000.0 / std::max(1.0, seq->frameRate.toDouble());
            emaMs_ = emaMs_ <= 0 ? ms : emaMs_ * 0.8 + ms * 0.2;
            if (emaMs_ > budget * 0.9 && autoDivisor_ < 8) {
                autoDivisor_ *= 2;
                emaMs_ *= 0.5;
                fastFrames_ = 0;
                AVC_DEBUG("preview", "adaptive quality: 1/{} ({:.1f} ms > {:.1f} ms budget)", autoDivisor_, ms, budget);
            } else if (emaMs_ < budget * 0.3 && autoDivisor_ > 1) {
                if (++fastFrames_ > static_cast<int>(2 * seq->frameRate.toDouble())) {
                    autoDivisor_ /= 2;
                    emaMs_ *= 2.0;
                    fastFrames_ = 0;
                }
            } else {
                fastFrames_ = 0;
            }
        } else if (!req.playing) {
            emaMs_ = emaMs_ <= 0 ? ms : emaMs_ * 0.8 + ms * 0.2;
        }
        {
            std::lock_guard lk(mutex_);
            stats_.avgRenderMs = emaMs_;
            stats_.autoDivisor = autoDivisor_;
            ++stats_.rendered;
        }
        finish(&f);
    }
}

}  // namespace avc::ui
