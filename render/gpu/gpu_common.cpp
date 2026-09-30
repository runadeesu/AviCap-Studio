#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "decode/video_frame.h"
#include "render/gpu/gpu.h"

namespace avc::gpu {

int bytesPerPixel(TexFormat f) {
    switch (f) {
    case TexFormat::RGBA8: return 4;
    case TexFormat::RGBA16: return 8;
    case TexFormat::RGBA16F: return 8;
    case TexFormat::RGBA32F: return 16;
    case TexFormat::R8: return 1;
    case TexFormat::RG8: return 2;
    case TexFormat::R16: return 2;
    case TexFormat::RG16: return 4;
    }
    return 4;
}

const char* texFormatName(TexFormat f) {
    switch (f) {
    case TexFormat::RGBA8: return "RGBA8";
    case TexFormat::RGBA16: return "RGBA16";
    case TexFormat::RGBA16F: return "RGBA16F";
    case TexFormat::RGBA32F: return "RGBA32F";
    case TexFormat::R8: return "R8";
    case TexFormat::RG8: return "RG8";
    case TexFormat::R16: return "R16";
    case TexFormat::RG16: return "RG16";
    }
    return "?";
}

const char* kernelSourceName(Kernel k) {
    switch (k) {
    case Kernel::YuvToRgb: return "yuv_to_rgb";
    case Kernel::Composite: return "composite";
    case Kernel::Fill: return "fill";
    case Kernel::Copy: return "copy";
    case Kernel::Mix: return "mix";
    case Kernel::RgbToYuv: return "rgb_to_yuv";
    case Kernel::BlurGaussian: return "blur_gaussian";
    case Kernel::BlurDirectional: return "blur_directional";
    case Kernel::BlurRadial: return "blur_radial";
    case Kernel::Sharpen: return "sharpen";
    case Kernel::Threshold: return "threshold";
    case Kernel::Combine: return "combine";
    case Kernel::Vignette: return "vignette";
    case Kernel::Noise: return "noise";
    case Kernel::Pixelate: return "pixelate";
    case Kernel::RgbSplit: return "rgb_split";
    case Kernel::LensDistort: return "lens_distort";
    case Kernel::Glitch: return "glitch";
    case Kernel::Shake: return "shake";
    case Kernel::ColorBasic: return "color_basic";
    case Kernel::ColorWheels: return "color_wheels";
    case Kernel::Curves: return "curves";
    case Kernel::Lut3D: return "lut3d";
    case Kernel::ChromaKey: return "chroma_key";
    case Kernel::Mask: return "mask";
    case Kernel::Dilate: return "dilate";
    case Kernel::TextCompose: return "text_compose";
    case Kernel::Transition: return "transition";
    case Kernel::Count: break;
    }
    return "";
}

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++h;  // round
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h;  // round half up
    return static_cast<uint16_t>(h);
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            x = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// ---------------------------------------------------------------- TexturePool

TexturePtr TexturePool::acquire(int width, int height, TexFormat format) {
    width = std::max(1, width);
    height = std::max(1, height);
    const uint64_t key = (static_cast<uint64_t>(width) << 36) | (static_cast<uint64_t>(height) << 8) | static_cast<uint64_t>(format);
    TexturePtr tex;
    {
        std::lock_guard lock(state_->mutex);
        auto it = state_->idle.find(key);
        if (it != state_->idle.end() && !it->second.empty()) {
            tex = std::move(it->second.back());
            it->second.pop_back();
            state_->idleBytes -= tex->bytes();
        }
    }
    if (!tex) {
        tex = device_.createTexture(TextureDesc{width, height, format, true});
        if (!tex) return nullptr;
    }
    std::weak_ptr<State> weak = state_;
    Texture* raw = tex.get();
    // Hand out an aliasing handle; when released the texture returns to the pool.
    return TexturePtr(raw, [weak, tex, key](Texture*) mutable {
        if (auto st = weak.lock()) {
            std::lock_guard lock(st->mutex);
            auto& v = st->idle[key];
            if (v.size() < 8) {
                st->idleBytes += tex->bytes();
                v.push_back(std::move(tex));
            }
        }
        tex.reset();
    });
}

void TexturePool::trim() {
    std::lock_guard lock(state_->mutex);
    state_->idle.clear();
    state_->idleBytes = 0;
}

size_t TexturePool::idleBytes() const {
    std::lock_guard lock(state_->mutex);
    return state_->idleBytes;
}

// ---------------------------------------------------------------- frame upload

namespace {

void yuvCoefficients(ColorMatrix m, float out[4]) {
    float kr = 0.2126f, kb = 0.0722f;
    if (m == ColorMatrix::BT601) {
        kr = 0.299f;
        kb = 0.114f;
    } else if (m == ColorMatrix::BT2020) {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    const float kg = 1.0f - kr - kb;
    out[0] = 2.0f * (1.0f - kr);
    out[1] = -2.0f * kb * (1.0f - kb) / kg;
    out[2] = -2.0f * kr * (1.0f - kr) / kg;
    out[3] = 2.0f * (1.0f - kb);
}

TexturePtr uploadPlane(TexturePool& pool, const uint8_t* data, int stride, int w, int h, TexFormat fmt) {
    TexturePtr t = pool.acquire(w, h, fmt);
    if (t) pool.device().upload(*t, data, stride);
    return t;
}

}  // namespace

TexturePtr uploadVideoFrame(Device& device, TexturePool& pool, const VideoFrame& f) {
    KernelParams p;
    float k[4];
    yuvCoefficients(f.matrix, k);
    p.set(1, k[0], k[1], k[2], k[3]);
    const bool bt2020 = f.primaries == ColorPrimaries::BT2020 || f.matrix == ColorMatrix::BT2020;
    const float transfer = f.transfer == ColorTransfer::PQ ? 1.0f : f.transfer == ColorTransfer::HLG ? 2.0f : 0.0f;
    p.set(2, f.premultipliedAlpha ? 0.0f : 1.0f, transfer, bt2020 ? 1.0f : 0.0f, 0.0f);
    p.set(3, 1.0f, 1.0f);
    TexturePtr planes[4];
    float mode = 1.0f;
    float scale = 1.0f;
    bool alpha = false;
    if (f.isHardware()) {
        if (!device.importHardwareFrame(f, planes[0], planes[1])) {
            AVC_WARN("render", "Hardware frame import failed");
            return nullptr;
        }
        mode = 2.0f;
        // Decoder surfaces are padded (e.g. 1088 rows): sample only the picture.
        p.set(3, static_cast<float>(f.width) / static_cast<float>(planes[0]->width()),
              static_cast<float>(f.height) / static_cast<float>(planes[0]->height()));
    } else {
        switch (f.format) {
        case PixelFormat::RGBA8:
        case PixelFormat::BGRA8:
            planes[0] = uploadPlane(pool, f.planes[0], f.strides[0], f.width, f.height, TexFormat::RGBA8);
            mode = f.format == PixelFormat::RGBA8 ? 0.0f : 4.0f;
            break;
        case PixelFormat::RGBA16:
            planes[0] = uploadPlane(pool, f.planes[0], f.strides[0], f.width, f.height, TexFormat::RGBA16);
            mode = 0.0f;
            break;
        case PixelFormat::GRAY8:
            planes[0] = uploadPlane(pool, f.planes[0], f.strides[0], f.width, f.height, TexFormat::R8);
            mode = 3.0f;
            break;
        case PixelFormat::NV12:
        case PixelFormat::P010: {
            const TexFormat yf = f.format == PixelFormat::NV12 ? TexFormat::R8 : TexFormat::R16;
            const TexFormat cf = f.format == PixelFormat::NV12 ? TexFormat::RG8 : TexFormat::RG16;
            planes[0] = uploadPlane(pool, f.planes[0], f.strides[0], f.planeWidth(0), f.planeHeight(0), yf);
            planes[1] = uploadPlane(pool, f.planes[1], f.strides[1], f.planeWidth(1), f.planeHeight(1), cf);
            mode = 2.0f;
            break;
        }
        case PixelFormat::YUV420P:
        case PixelFormat::YUV422P:
        case PixelFormat::YUV444P:
        case PixelFormat::YUVA420P:
        case PixelFormat::YUV420P10:
        case PixelFormat::YUV422P10:
        case PixelFormat::YUV444P10: {
            const bool wide = is16Bit(f.format);
            const TexFormat pf = wide ? TexFormat::R16 : TexFormat::R8;
            if (wide) scale = 65535.0f / static_cast<float>((1 << std::clamp(f.bitDepth, 8, 16)) - 1);
            const int n = planeCount(f.format);
            for (int i = 0; i < n; ++i)
                planes[i] = uploadPlane(pool, f.planes[static_cast<size_t>(i)], f.strides[static_cast<size_t>(i)],
                                        f.planeWidth(i), f.planeHeight(i), pf);
            alpha = f.format == PixelFormat::YUVA420P;
            mode = 1.0f;
            break;
        }
        case PixelFormat::None: return nullptr;
        }
    }
    if (!planes[0]) return nullptr;
    p.set(0, mode, f.fullRange ? 1.0f : 0.0f, scale, alpha ? 1.0f : 0.0f);
    TexturePtr out = pool.acquire(f.width, f.height, TexFormat::RGBA16F);
    if (!out) return nullptr;
    device.run(Kernel::YuvToRgb, p, {planes[0].get(), planes[1].get(), planes[2].get(), planes[3].get()}, *out);
    return out;
}

std::vector<uint8_t> readbackRgba8(Device& device, const Texture& tex, bool unpremultiply) {
    const int w = tex.width(), h = tex.height();
    std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    std::vector<float> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    if (tex.format() == TexFormat::RGBA8) {
        device.readback(tex, out.data(), w * 4);
        for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = out[i] / 255.0f;
    } else if (tex.format() == TexFormat::RGBA16F) {
        std::vector<uint16_t> halfs(rgba.size());
        device.readback(tex, halfs.data(), w * 8);
        for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = halfToFloat(halfs[i]);
    } else if (tex.format() == TexFormat::RGBA32F) {
        device.readback(tex, rgba.data(), w * 16);
    } else {
        return out;
    }
    for (size_t i = 0; i < rgba.size(); i += 4) {
        float a = std::clamp(rgba[i + 3], 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) {
            float v = rgba[i + static_cast<size_t>(c)];
            if (unpremultiply && a > 0.0f) v /= a;
            out[i + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        out[i + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
    }
    return out;
}

void deviceLockThunk(void* device) { static_cast<Device*>(device)->lock(); }
void deviceUnlockThunk(void* device) { static_cast<Device*>(device)->unlock(); }

}  // namespace avc::gpu
