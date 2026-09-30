#pragma once
// Video decoding interface (IVideoDecoder) and the FFmpeg implementation.
//
// frameAt(t) returns the frame that is on screen at media time t, i.e. the
// frame with the greatest pts <= t. Sequential requests decode forward;
// distant or backward requests seek to the preceding keyframe. Decoders are
// NOT thread-safe: each playback/export/thumbnail consumer owns its instance.

#include <memory>
#include <string>

#include "core/jobs.h"
#include "core/result.h"
#include "decode/video_frame.h"
#include "timeline/model.h"

namespace avc {

enum class HwDecodeMode {
    Off,       // software only
    Download,  // hardware decode, frames copied to system memory (CPU consumers)
    Keep,      // hardware decode, frames stay on the GPU (zero-copy preview)
};

struct VideoDecoderOptions {
    int streamIndex = -1;  // -1: best video stream
    HwDecodeMode hardware = HwDecodeMode::Off;
    void* d3d11Device = nullptr;  // ID3D11Device* shared with the renderer (Keep mode)
    int threads = 0;              // 0: automatic
    // When > 0, frames are downscaled on decode to fit this box (thumbnails,
    // analysis) and converted to RGBA8.
    int maxWidth = 0;
    int maxHeight = 0;
    bool fastDecode = false;  // skip loop filter etc. (scrubbing / analysis)
};

struct DecoderStats {
    uint64_t framesDecoded = 0;
    uint64_t seeks = 0;
    uint64_t packetsRead = 0;
    double lastDecodeMs = 0;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual Result<VideoFramePtr> frameAt(Time t, const CancelToken& cancel = {}) = 0;
    // Decodes the next frame after the last returned one (fast sequential reads,
    // analysis). Returns an error at end of stream.
    virtual Result<VideoFramePtr> nextFrame(const CancelToken& cancel = {}) = 0;
    [[nodiscard]] virtual const VideoStreamInfo& stream() const = 0;
    [[nodiscard]] virtual Time duration() const = 0;
    [[nodiscard]] virtual bool hardwareAccelerated() const = 0;
    [[nodiscard]] virtual std::string decoderName() const = 0;
    [[nodiscard]] virtual const DecoderStats& stats() const = 0;
};

Result<std::unique_ptr<IVideoDecoder>> openVideoDecoder(const std::string& utf8Path, const VideoDecoderOptions& opt = {});

// Hardware decode availability summary for diagnostics.
std::string hardwareDecodeSummary();

}  // namespace avc
