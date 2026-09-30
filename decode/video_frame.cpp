#include "decode/video_frame.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace avc {

const char* pixelFormatName(PixelFormat f) {
    switch (f) {
    case PixelFormat::None: return "none";
    case PixelFormat::YUV420P: return "yuv420p";
    case PixelFormat::YUV422P: return "yuv422p";
    case PixelFormat::YUV444P: return "yuv444p";
    case PixelFormat::YUV420P10: return "yuv420p16";
    case PixelFormat::YUV422P10: return "yuv422p16";
    case PixelFormat::YUV444P10: return "yuv444p16";
    case PixelFormat::NV12: return "nv12";
    case PixelFormat::P010: return "p010";
    case PixelFormat::RGBA8: return "rgba";
    case PixelFormat::BGRA8: return "bgra";
    case PixelFormat::RGBA16: return "rgba64";
    case PixelFormat::GRAY8: return "gray";
    case PixelFormat::YUVA420P: return "yuva420p";
    }
    return "?";
}

int planeCount(PixelFormat f) {
    switch (f) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV444P:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P10:
    case PixelFormat::YUV444P10: return 3;
    case PixelFormat::YUVA420P: return 4;
    case PixelFormat::NV12:
    case PixelFormat::P010: return 2;
    case PixelFormat::None: return 0;
    default: return 1;
    }
}

bool isYuv(PixelFormat f) {
    return f != PixelFormat::RGBA8 && f != PixelFormat::BGRA8 && f != PixelFormat::RGBA16 && f != PixelFormat::None &&
           f != PixelFormat::GRAY8;
}

bool isSemiPlanar(PixelFormat f) { return f == PixelFormat::NV12 || f == PixelFormat::P010; }

bool is16Bit(PixelFormat f) {
    return f == PixelFormat::YUV420P10 || f == PixelFormat::YUV422P10 || f == PixelFormat::YUV444P10 ||
           f == PixelFormat::P010 || f == PixelFormat::RGBA16;
}

int chromaShiftX(PixelFormat f) {
    switch (f) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P10:
    case PixelFormat::NV12:
    case PixelFormat::P010:
    case PixelFormat::YUVA420P: return 1;
    default: return 0;
    }
}

int chromaShiftY(PixelFormat f) {
    switch (f) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
    case PixelFormat::NV12:
    case PixelFormat::P010:
    case PixelFormat::YUVA420P: return 1;
    default: return 0;
    }
}

int VideoFrame::planeWidth(int plane) const noexcept {
    if (plane == 0 || plane == 3 || !isYuv(format)) return width;
    return (width + (1 << chromaShiftX(format)) - 1) >> chromaShiftX(format);
}

int VideoFrame::planeHeight(int plane) const noexcept {
    if (plane == 0 || plane == 3 || !isYuv(format)) return height;
    return (height + (1 << chromaShiftY(format)) - 1) >> chromaShiftY(format);
}

size_t VideoFrame::byteSize() const noexcept {
    if (isHardware()) return static_cast<size_t>(width) * static_cast<size_t>(height) * 2;  // approx. surface cost
    size_t total = 0;
    for (int p = 0; p < planeCount(format); ++p)
        total += static_cast<size_t>(std::abs(strides[static_cast<size_t>(p)])) * static_cast<size_t>(planeHeight(p));
    return total;
}

uint64_t nextFrameSerial() {
    static std::atomic<uint64_t> serial{1};
    return serial.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<VideoFrame> allocateFrame(int width, int height, PixelFormat format) {
    auto f = std::make_shared<VideoFrame>();
    f->width = width;
    f->height = height;
    f->format = format;
    f->serial = nextFrameSerial();
    const int bpc = is16Bit(format) ? 2 : 1;
    size_t offsets[4] = {};
    size_t total = 0;
    for (int p = 0; p < planeCount(format); ++p) {
        int comps = 1;
        if (format == PixelFormat::RGBA8 || format == PixelFormat::BGRA8 || format == PixelFormat::RGBA16) comps = 4;
        else if (isSemiPlanar(format) && p == 1) comps = 2;
        const int pw = (p == 0 || p == 3 || !isYuv(format)) ? width : ((width + (1 << chromaShiftX(format)) - 1) >> chromaShiftX(format));
        const int ph = (p == 0 || p == 3 || !isYuv(format)) ? height : ((height + (1 << chromaShiftY(format)) - 1) >> chromaShiftY(format));
        const int stride = ((pw * comps * bpc) + 31) & ~31;
        f->strides[static_cast<size_t>(p)] = stride;
        offsets[p] = total;
        total += static_cast<size_t>(stride) * static_cast<size_t>(ph);
    }
    auto buffer = std::shared_ptr<uint8_t>(static_cast<uint8_t*>(std::calloc(total + 64, 1)), std::free);
    uint8_t* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(buffer.get()) + 31) & ~uintptr_t(31));
    for (int p = 0; p < planeCount(format); ++p) f->planes[static_cast<size_t>(p)] = base + offsets[p];
    f->owner = buffer;
    return f;
}

namespace {

struct YuvCoeffs {
    float rv, gu, gv, bu;
};

YuvCoeffs coeffsFor(ColorMatrix m) {
    float kr = 0.2126f, kb = 0.0722f;
    if (m == ColorMatrix::BT601) {
        kr = 0.299f;
        kb = 0.114f;
    } else if (m == ColorMatrix::BT2020) {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    const float kg = 1.0f - kr - kb;
    return {2.0f * (1.0f - kr), -2.0f * kb * (1.0f - kb) / kg, -2.0f * kr * (1.0f - kr) / kg, 2.0f * (1.0f - kb)};
}

inline uint8_t clamp8(float v) { return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

}  // namespace

void convertToRgba8(const VideoFrame& f, uint8_t* dst, int dstStride) {
    const int w = f.width, h = f.height;
    if (f.format == PixelFormat::RGBA8 || f.format == PixelFormat::BGRA8) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = f.planes[0] + static_cast<ptrdiff_t>(y) * f.strides[0];
            uint8_t* d = dst + static_cast<ptrdiff_t>(y) * dstStride;
            if (f.format == PixelFormat::RGBA8) {
                std::memcpy(d, s, static_cast<size_t>(w) * 4);
            } else {
                for (int x = 0; x < w; ++x) {
                    d[x * 4 + 0] = s[x * 4 + 2];
                    d[x * 4 + 1] = s[x * 4 + 1];
                    d[x * 4 + 2] = s[x * 4 + 0];
                    d[x * 4 + 3] = s[x * 4 + 3];
                }
            }
        }
        return;
    }
    if (f.format == PixelFormat::RGBA16) {
        for (int y = 0; y < h; ++y) {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(f.planes[0] + static_cast<ptrdiff_t>(y) * f.strides[0]);
            uint8_t* d = dst + static_cast<ptrdiff_t>(y) * dstStride;
            for (int x = 0; x < w * 4; ++x) d[x] = static_cast<uint8_t>(s[x] >> 8);
        }
        return;
    }
    if (f.format == PixelFormat::GRAY8) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = f.planes[0] + static_cast<ptrdiff_t>(y) * f.strides[0];
            uint8_t* d = dst + static_cast<ptrdiff_t>(y) * dstStride;
            for (int x = 0; x < w; ++x) {
                d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = s[x];
                d[x * 4 + 3] = 255;
            }
        }
        return;
    }
    const YuvCoeffs k = coeffsFor(f.matrix);
    const bool wide = is16Bit(f.format);
    const float maxv = wide ? (f.format == PixelFormat::P010 ? 65535.0f : static_cast<float>((1 << f.bitDepth) - 1)) : 255.0f;
    const float yOff = f.fullRange ? 0.0f : 16.0f / 255.0f;
    const float yScale = f.fullRange ? 1.0f : 255.0f / 219.0f;
    const float cScale = f.fullRange ? 1.0f : 255.0f / 224.0f;
    const int sx = chromaShiftX(f.format), sy = chromaShiftY(f.format);
    auto sample = [&](int plane, int x, int y, int comp, int comps) -> float {
        const uint8_t* row = f.planes[static_cast<size_t>(plane)] + static_cast<ptrdiff_t>(y) * f.strides[static_cast<size_t>(plane)];
        if (wide) return static_cast<float>(reinterpret_cast<const uint16_t*>(row)[x * comps + comp]) / maxv;
        return static_cast<float>(row[x * comps + comp]) / 255.0f;
    };
    for (int y = 0; y < h; ++y) {
        uint8_t* d = dst + static_cast<ptrdiff_t>(y) * dstStride;
        const int cy = y >> sy;
        for (int x = 0; x < w; ++x) {
            const int cx = x >> sx;
            float Y = sample(0, x, y, 0, 1);
            float U, V;
            if (isSemiPlanar(f.format)) {
                U = sample(1, cx, cy, 0, 2);
                V = sample(1, cx, cy, 1, 2);
            } else {
                U = sample(1, cx, cy, 0, 1);
                V = sample(2, cx, cy, 0, 1);
            }
            Y = (Y - yOff) * yScale;
            U = (U - 0.5f) * cScale;
            V = (V - 0.5f) * cScale;
            d[x * 4 + 0] = clamp8(Y + k.rv * V);
            d[x * 4 + 1] = clamp8(Y + k.gu * U + k.gv * V);
            d[x * 4 + 2] = clamp8(Y + k.bu * U);
            d[x * 4 + 3] = f.format == PixelFormat::YUVA420P ? f.planes[3][static_cast<ptrdiff_t>(y) * f.strides[3] + x] : 255;
        }
    }
}

}  // namespace avc
