#pragma once
// Preview renderer: renders the program monitor on a dedicated thread so the
// UI thread never waits for decoding or compositing.
//
// The UI posts requests (latest wins); the render thread composites the
// newest one on the shared GPU device and publishes the resulting texture.
// In Auto quality the resolution adapts during playback: when frames take
// longer than the frame budget the preview drops to 1/2, 1/4, 1/8 resolution
// and climbs back when there is headroom. A paused frame is always refined
// to the best quality that fits the viewer.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cache/cache.h"
#include "render/compositor.h"
#include "render/frame_provider.h"
#include "render/gpu/gpu.h"
#include "text/text_raster.h"
#include "timeline/model.h"

namespace avc::ui {

enum class PreviewQuality { Auto, Full, Half, Quarter, Eighth };
const char* previewQualityName(PreviewQuality q);  // settings id: "auto", "full", ...
PreviewQuality previewQualityFromName(const std::string& s);

struct PreviewRequest {
    ProjectPtr project;
    SequenceId sequence = kInvalidId;
    Time time;
    int viewportWidth = 0;  // pixels available in the viewer (0 = unknown)
    int viewportHeight = 0;
    PreviewQuality quality = PreviewQuality::Auto;
    bool playing = false;
    bool wantScopes = false;
};

struct PreviewFrame {
    gpu::TexturePtr texture;      // premultiplied RGBA16F (opaque background)
    Time time;
    const Project* project = nullptr;  // identity only
    SequenceId sequence = kInvalidId;
    int width = 0;
    int height = 0;
    int divisor = 1;              // 1 = full resolution, 2 = half, ...
    double renderMs = 0;
    int missingFrames = 0;        // layers whose media could not be decoded
    std::string error;
    std::shared_ptr<const cache::Image> scopes;  // small RGBA8 copy for video scopes
    uint64_t serial = 0;
};

struct PreviewStats {
    double avgRenderMs = 0;
    int autoDivisor = 1;
    uint64_t rendered = 0;
    uint64_t superseded = 0;  // requests replaced before they were rendered (dropped frames while playing)
    size_t openDecoders = 0;
    std::string decoder;
};

class PreviewRenderer {
public:
    PreviewRenderer(gpu::Device& device, std::shared_ptr<text::ITextRasterizer> text, FrameProviderOptions frames);
    ~PreviewRenderer();
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    void request(PreviewRequest r);
    [[nodiscard]] PreviewFrame latest() const;
    [[nodiscard]] PreviewStats stats() const;
    // Waits until the newest request has been rendered (tests, screenshots).
    bool waitIdle(int timeoutMs);
    // Forces the next request to render even if nothing changed.
    void invalidate();
    // Called on the render thread after each published frame.
    void setOnFrame(std::function<void()> fn);
    MediaFrameProvider& frames() { return *frames_; }
    void trimMemory();

private:
    void loop();
    int chooseDivisor(const PreviewRequest& r, const Sequence& seq);

    gpu::Device& device_;
    std::unique_ptr<MediaFrameProvider> frames_;
    std::unique_ptr<Compositor> compositor_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    PreviewRequest pending_;
    uint64_t pendingSerial_ = 0;
    uint64_t renderedSerial_ = 0;
    bool hasPending_ = false;
    bool stop_ = false;
    bool force_ = false;
    PreviewFrame latest_;
    PreviewStats stats_;
    std::function<void()> onFrame_;
    std::condition_variable idleCv_;

    // Render-thread state
    int autoDivisor_ = 1;
    int fastFrames_ = 0;
    double emaMs_ = 0;
    std::thread thread_;
};

}  // namespace avc::ui
