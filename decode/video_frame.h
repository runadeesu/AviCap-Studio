#pragma once
// Decoded video frame (CPU planes or a hardware surface reference).

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "core/time.h"

namespace avc {

enum class PixelFormat : uint8_t {
    None,
    YUV420P,    // 8-bit planar
    YUV422P,
    YUV444P,
    YUV420P10,  // 10..16-bit planar (little endian, stored in 16-bit words, MSB-normalized = false)
    YUV422P10,
    YUV444P10,
    NV12,       // 8-bit semi-planar
    P010,       // 10-bit semi-planar, MSB aligned in 16-bit words
    RGBA8,
    BGRA8,
    RGBA16,     // 16-bit per channel RGBA
    GRAY8,
    YUVA420P,   // 8-bit planar with alpha
};

const char* pixelFormatName(PixelFormat f);
int planeCount(PixelFormat f);
bool isYuv(PixelFormat f);
bool isSemiPlanar(PixelFormat f);
bool is16Bit(PixelFormat f);
// Chroma subsampling shifts (log2) for planar YUV formats.
int chromaShiftX(PixelFormat f);
int chromaShiftY(PixelFormat f);

enum class ColorMatrix : uint8_t { BT601, BT709, BT2020 };
enum class ColorTransfer : uint8_t { SDR, PQ, HLG, Linear, SRGB };
enum class ColorPrimaries : uint8_t { BT709, BT2020, DisplayP3, BT601 };

enum class HwFrameKind : uint8_t { None, D3D11 };

struct VideoFrame {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::None;
    std::array<const uint8_t*, 4> planes{};
    std::array<int, 4> strides{};
    int bitDepth = 8;  // significant bits for 16-bit containers (10, 12, 16)

    ColorMatrix matrix = ColorMatrix::BT709;
    bool fullRange = false;
    ColorTransfer transfer = ColorTransfer::SDR;
    ColorPrimaries primaries = ColorPrimaries::BT709;
    Rational sampleAspect{1, 1};
    int rotation = 0;
    bool premultipliedAlpha = false;

    Time pts;       // media time of this frame
    Time duration;  // display duration

    // Hardware surface (zero-copy path). `hwTexture` is an ID3D11Texture2D*
    // and `hwSubresource` the texture-array slice; `owner` keeps it alive.
    HwFrameKind hw = HwFrameKind::None;
    void* hwTexture = nullptr;
    int hwSubresource = 0;
    PixelFormat hwFormat = PixelFormat::None;  // NV12 / P010 layout of the surface

    std::shared_ptr<void> owner;  // AVFrame / buffer lifetime
    uint64_t serial = 0;          // unique per decoded frame (cache/GPU upload key)

    [[nodiscard]] bool isHardware() const noexcept { return hw != HwFrameKind::None; }
    [[nodiscard]] bool covers(Time t) const noexcept { return t >= pts && t < pts + duration; }
    [[nodiscard]] size_t byteSize() const noexcept;
    [[nodiscard]] int planeWidth(int plane) const noexcept;
    [[nodiscard]] int planeHeight(int plane) const noexcept;
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

// Allocates a CPU frame with owned, 32-byte aligned planes.
std::shared_ptr<VideoFrame> allocateFrame(int width, int height, PixelFormat format);

// Converts any CPU frame to straight-alpha RGBA8 (used for thumbnails, tests,
// analysis and the software renderer). Output is tightly packed.
void convertToRgba8(const VideoFrame& src, uint8_t* dst, int dstStride);

uint64_t nextFrameSerial();

}  // namespace avc
