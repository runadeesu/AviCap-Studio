// CPU backend: runs the shared kernels (render/kernels/*.hlsl compiled as C++)
// over all pixels using a small worker pool. Used headless (tests, CLI on
// systems without a GPU) and as the last-resort fallback.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/platform.h"
#include "render/gpu/gpu.h"
#include "render/gpu/shader_compat.h"

// ---- kernels compiled as C++ ------------------------------------------------------
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#define AVC_KERNEL_NS(name) namespace avc::gpu::cpuk::name { using namespace ::avc::hlsl;
#define AVC_KERNEL_END }

AVC_KERNEL_NS(yuv_to_rgb)
#include "render/kernels/yuv_to_rgb.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(composite)
#include "render/kernels/composite.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(fill)
#include "render/kernels/fill.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(copy)
#include "render/kernels/copy.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(mix)
#include "render/kernels/mix.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(rgb_to_yuv)
#include "render/kernels/rgb_to_yuv.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(blur_gaussian)
#include "render/kernels/blur_gaussian.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(blur_directional)
#include "render/kernels/blur_directional.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(blur_radial)
#include "render/kernels/blur_radial.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(sharpen)
#include "render/kernels/sharpen.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(threshold)
#include "render/kernels/threshold.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(combine)
#include "render/kernels/combine.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(vignette)
#include "render/kernels/vignette.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(noise)
#include "render/kernels/noise.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(pixelate)
#include "render/kernels/pixelate.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(rgb_split)
#include "render/kernels/rgb_split.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(lens_distort)
#include "render/kernels/lens_distort.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(glitch)
#include "render/kernels/glitch.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(shake)
#include "render/kernels/shake.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(color_basic)
#include "render/kernels/color_basic.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(color_wheels)
#include "render/kernels/color_wheels.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(curves)
#include "render/kernels/curves.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(lut3d)
#include "render/kernels/lut3d.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(chroma_key)
#include "render/kernels/chroma_key.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(mask)
#include "render/kernels/mask.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(dilate)
#include "render/kernels/dilate.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(text_compose)
#include "render/kernels/text_compose.hlsl"
AVC_KERNEL_END
AVC_KERNEL_NS(transition)
#include "render/kernels/transition.hlsl"
AVC_KERNEL_END

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace avc::hlsl {
constinit thread_local const KernelContext* tls_ctx = nullptr;
}  // namespace avc::hlsl

namespace avc::gpu {

namespace {

using hlsl::float4;
using KernelFn = float4 (*)(const hlsl::KernelContext&);

KernelFn kernelFunction(Kernel k) {
    switch (k) {
    case Kernel::YuvToRgb: return &cpuk::yuv_to_rgb::kernel_main;
    case Kernel::Composite: return &cpuk::composite::kernel_main;
    case Kernel::Fill: return &cpuk::fill::kernel_main;
    case Kernel::Copy: return &cpuk::copy::kernel_main;
    case Kernel::Mix: return &cpuk::mix::kernel_main;
    case Kernel::RgbToYuv: return &cpuk::rgb_to_yuv::kernel_main;
    case Kernel::BlurGaussian: return &cpuk::blur_gaussian::kernel_main;
    case Kernel::BlurDirectional: return &cpuk::blur_directional::kernel_main;
    case Kernel::BlurRadial: return &cpuk::blur_radial::kernel_main;
    case Kernel::Sharpen: return &cpuk::sharpen::kernel_main;
    case Kernel::Threshold: return &cpuk::threshold::kernel_main;
    case Kernel::Combine: return &cpuk::combine::kernel_main;
    case Kernel::Vignette: return &cpuk::vignette::kernel_main;
    case Kernel::Noise: return &cpuk::noise::kernel_main;
    case Kernel::Pixelate: return &cpuk::pixelate::kernel_main;
    case Kernel::RgbSplit: return &cpuk::rgb_split::kernel_main;
    case Kernel::LensDistort: return &cpuk::lens_distort::kernel_main;
    case Kernel::Glitch: return &cpuk::glitch::kernel_main;
    case Kernel::Shake: return &cpuk::shake::kernel_main;
    case Kernel::ColorBasic: return &cpuk::color_basic::kernel_main;
    case Kernel::ColorWheels: return &cpuk::color_wheels::kernel_main;
    case Kernel::Curves: return &cpuk::curves::kernel_main;
    case Kernel::Lut3D: return &cpuk::lut3d::kernel_main;
    case Kernel::ChromaKey: return &cpuk::chroma_key::kernel_main;
    case Kernel::Mask: return &cpuk::mask::kernel_main;
    case Kernel::Dilate: return &cpuk::dilate::kernel_main;
    case Kernel::TextCompose: return &cpuk::text_compose::kernel_main;
    case Kernel::Transition: return &cpuk::transition::kernel_main;
    case Kernel::Count: break;
    }
    return nullptr;
}

class CpuTexture final : public Texture {
public:
    explicit CpuTexture(const TextureDesc& d)
        : Texture(d), px(static_cast<size_t>(d.width) * static_cast<size_t>(d.height)) {}
    std::vector<float4> px;
};

// Minimal fork-join pool for per-row parallelism.
class ParallelFor {
public:
    explicit ParallelFor(unsigned threads) {
        for (unsigned i = 0; i + 1 < threads; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~ParallelFor() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    void run(int count, const std::function<void(int, int)>& fn) {
        if (workers_.empty() || count < 64) {
            fn(0, count);
            return;
        }
        std::unique_lock lock(runMutex_);  // one job at a time
        {
            std::lock_guard l(m_);
            job_ = &fn;
            count_ = count;
            next_.store(0);
            active_ = static_cast<int>(workers_.size());
            ++generation_;
        }
        cv_.notify_all();
        work();
        std::unique_lock l(m_);
        done_.wait(l, [this] { return active_ == 0; });
        job_ = nullptr;
    }

private:
    static constexpr int kChunk = 16;
    void work() {
        for (;;) {
            const int start = next_.fetch_add(kChunk);
            if (start >= count_) break;
            (*job_)(start, std::min(count_, start + kChunk));
        }
    }
    void loop() {
        setCurrentThreadName("cpu-render");
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock l(m_);
                cv_.wait(l, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            work();
            {
                std::lock_guard l(m_);
                if (--active_ == 0) done_.notify_all();
            }
        }
    }
    std::vector<std::thread> workers_;
    std::mutex m_, runMutex_;
    std::condition_variable cv_, done_;
    const std::function<void(int, int)>* job_ = nullptr;
    int count_ = 0;
    std::atomic<int> next_{0};
    int active_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

float unorm8(uint8_t v) { return static_cast<float>(v) / 255.0f; }
float unorm16(uint16_t v) { return static_cast<float>(v) / 65535.0f; }
uint8_t toU8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
uint16_t toU16(float v) { return static_cast<uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); }

class CpuDevice final : public Device {
public:
    explicit CpuDevice(unsigned threads) : pool_(threads ? threads : std::max(1u, hardwareThreads())) {
        info_.kind = BackendKind::CPU;
        info_.name = "CPU";
        info_.adapter = cpuDescription();
        info_.vendor = "CPU";
        info_.software = true;
        empty_.width = 1;
        empty_.height = 1;
        empty_.data = &emptyPixel_;
    }

    const DeviceInfo& info() const override { return info_; }

    TexturePtr createTexture(const TextureDesc& desc) override {
        auto t = std::make_shared<CpuTexture>(desc);
        *memory_ += t->bytes();
        std::shared_ptr<std::atomic<size_t>> mem = memory_;
        const size_t b = t->bytes();
        return TexturePtr(t.get(), [t, mem, b](Texture*) mutable {
            *mem -= b;
            t.reset();
        });
    }

    void upload(Texture& tex, const void* data, int stride) override {
        auto& t = static_cast<CpuTexture&>(tex);
        const auto* bytes = static_cast<const uint8_t*>(data);
        const int w = t.width(), h = t.height();
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = bytes + static_cast<ptrdiff_t>(y) * stride;
            float4* dst = t.px.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
            for (int x = 0; x < w; ++x) {
                switch (t.format()) {
                case TexFormat::RGBA8:
                    dst[x] = float4(unorm8(row[x * 4]), unorm8(row[x * 4 + 1]), unorm8(row[x * 4 + 2]), unorm8(row[x * 4 + 3]));
                    break;
                case TexFormat::RGBA16: {
                    const auto* r = reinterpret_cast<const uint16_t*>(row);
                    dst[x] = float4(unorm16(r[x * 4]), unorm16(r[x * 4 + 1]), unorm16(r[x * 4 + 2]), unorm16(r[x * 4 + 3]));
                    break;
                }
                case TexFormat::RGBA16F: {
                    const auto* r = reinterpret_cast<const uint16_t*>(row);
                    dst[x] = float4(halfToFloat(r[x * 4]), halfToFloat(r[x * 4 + 1]), halfToFloat(r[x * 4 + 2]),
                                    halfToFloat(r[x * 4 + 3]));
                    break;
                }
                case TexFormat::RGBA32F: {
                    const auto* r = reinterpret_cast<const float*>(row);
                    dst[x] = float4(r[x * 4], r[x * 4 + 1], r[x * 4 + 2], r[x * 4 + 3]);
                    break;
                }
                case TexFormat::R8: dst[x] = float4(unorm8(row[x]), 0.0f, 0.0f, 1.0f); break;
                case TexFormat::RG8: dst[x] = float4(unorm8(row[x * 2]), unorm8(row[x * 2 + 1]), 0.0f, 1.0f); break;
                case TexFormat::R16:
                    dst[x] = float4(unorm16(reinterpret_cast<const uint16_t*>(row)[x]), 0.0f, 0.0f, 1.0f);
                    break;
                case TexFormat::RG16: {
                    const auto* r = reinterpret_cast<const uint16_t*>(row);
                    dst[x] = float4(unorm16(r[x * 2]), unorm16(r[x * 2 + 1]), 0.0f, 1.0f);
                    break;
                }
                }
            }
        }
    }

    void run(Kernel k, const KernelParams& params, std::initializer_list<const Texture*> inputs, Texture& target) override {
        KernelFn fn = kernelFunction(k);
        if (!fn) return;
        auto& out = static_cast<CpuTexture&>(target);
        hlsl::Texture views[kMaxInputs];
        int i = 0;
        for (const Texture* in : inputs) {
            if (i >= kMaxInputs) break;
            if (in) {
                const auto& ct = static_cast<const CpuTexture&>(*in);
                views[i] = hlsl::Texture{ct.px.data(), ct.width(), ct.height()};
            } else {
                views[i] = empty_;
            }
            ++i;
        }
        for (; i < kMaxInputs; ++i) views[i] = empty_;
        const int w = out.width(), h = out.height();
        const auto* p = reinterpret_cast<const float4*>(params.v.data());
        const bool eightBit = out.format() == TexFormat::RGBA8 || out.format() == TexFormat::R8 || out.format() == TexFormat::RG8;
        std::function<void(int, int)> job = [&](int y0, int y1) {
            hlsl::KernelContext ctx;
            ctx.size = hlsl::float2(static_cast<float>(w), static_cast<float>(h));
            ctx.params = p;
            for (int t = 0; t < kMaxInputs; ++t) ctx.tex[t] = &views[t];
            const hlsl::KernelContext* prev = hlsl::tls_ctx;
            hlsl::tls_ctx = &ctx;
            for (int y = y0; y < y1; ++y) {
                float4* row = out.px.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
                ctx.pos.y = static_cast<float>(y) + 0.5f;
                ctx.uv.y = ctx.pos.y / ctx.size.y;
                for (int x = 0; x < w; ++x) {
                    ctx.pos.x = static_cast<float>(x) + 0.5f;
                    ctx.uv.x = ctx.pos.x / ctx.size.x;
                    float4 v = fn(ctx);
                    if (eightBit) {
                        v = float4(std::round(std::clamp(v.x, 0.0f, 1.0f) * 255.0f) / 255.0f,
                                   std::round(std::clamp(v.y, 0.0f, 1.0f) * 255.0f) / 255.0f,
                                   std::round(std::clamp(v.z, 0.0f, 1.0f) * 255.0f) / 255.0f,
                                   std::round(std::clamp(v.w, 0.0f, 1.0f) * 255.0f) / 255.0f);
                    }
                    row[x] = v;
                }
            }
            hlsl::tls_ctx = prev;
        };
        pool_.run(h, job);
    }

    void clear(Texture& tex, float r, float g, float b, float a) override {
        auto& t = static_cast<CpuTexture&>(tex);
        std::fill(t.px.begin(), t.px.end(), float4(r, g, b, a));
    }

    void readback(const Texture& tex, void* dst, int stride) override {
        const auto& t = static_cast<const CpuTexture&>(tex);
        auto* bytes = static_cast<uint8_t*>(dst);
        const int w = t.width(), h = t.height();
        for (int y = 0; y < h; ++y) {
            uint8_t* row = bytes + static_cast<ptrdiff_t>(y) * stride;
            const float4* src = t.px.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
            for (int x = 0; x < w; ++x) {
                const float4 v = src[x];
                switch (t.format()) {
                case TexFormat::RGBA8:
                    row[x * 4] = toU8(v.x);
                    row[x * 4 + 1] = toU8(v.y);
                    row[x * 4 + 2] = toU8(v.z);
                    row[x * 4 + 3] = toU8(v.w);
                    break;
                case TexFormat::RGBA16: {
                    auto* r = reinterpret_cast<uint16_t*>(row);
                    r[x * 4] = toU16(v.x);
                    r[x * 4 + 1] = toU16(v.y);
                    r[x * 4 + 2] = toU16(v.z);
                    r[x * 4 + 3] = toU16(v.w);
                    break;
                }
                case TexFormat::RGBA16F: {
                    auto* r = reinterpret_cast<uint16_t*>(row);
                    r[x * 4] = floatToHalf(v.x);
                    r[x * 4 + 1] = floatToHalf(v.y);
                    r[x * 4 + 2] = floatToHalf(v.z);
                    r[x * 4 + 3] = floatToHalf(v.w);
                    break;
                }
                case TexFormat::RGBA32F: {
                    auto* r = reinterpret_cast<float*>(row);
                    r[x * 4] = v.x;
                    r[x * 4 + 1] = v.y;
                    r[x * 4 + 2] = v.z;
                    r[x * 4 + 3] = v.w;
                    break;
                }
                case TexFormat::R8: row[x] = toU8(v.x); break;
                case TexFormat::RG8:
                    row[x * 2] = toU8(v.x);
                    row[x * 2 + 1] = toU8(v.y);
                    break;
                case TexFormat::R16: reinterpret_cast<uint16_t*>(row)[x] = toU16(v.x); break;
                case TexFormat::RG16:
                    reinterpret_cast<uint16_t*>(row)[x * 2] = toU16(v.x);
                    reinterpret_cast<uint16_t*>(row)[x * 2 + 1] = toU16(v.y);
                    break;
                }
            }
        }
    }

    size_t textureMemoryInUse() const override { return memory_->load(); }

private:
    DeviceInfo info_;
    ParallelFor pool_;
    std::shared_ptr<std::atomic<size_t>> memory_ = std::make_shared<std::atomic<size_t>>(0);
    float4 emptyPixel_{0.0f, 0.0f, 0.0f, 0.0f};
    hlsl::Texture empty_;
};

}  // namespace

std::unique_ptr<Device> createCpuDevice(unsigned threads) { return std::make_unique<CpuDevice>(threads); }

}  // namespace avc::gpu
