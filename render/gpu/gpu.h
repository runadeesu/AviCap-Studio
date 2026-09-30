#pragma once
// GPU abstraction used by the compositor, effects, text and export.
//
// A Device creates textures, runs kernels (full-screen pixel passes written in
// the shared HLSL subset, see render/kernels) and reads results back.
// Backends: Direct3D 11 (Windows, default), CPU (headless tests and last-resort
// fallback). Direct3D 12 plugs in behind the same interface.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace avc {
struct VideoFrame;
}

namespace avc::gpu {

enum class TexFormat : uint8_t {
    RGBA8,    // UNORM
    RGBA16,   // UNORM (16-bit per channel sources)
    RGBA16F,  // half float: working format for compositing
    RGBA32F,
    R8,
    RG8,
    R16,
    RG16,
};

int bytesPerPixel(TexFormat f);
const char* texFormatName(TexFormat f);

struct TextureDesc {
    int width = 0;
    int height = 0;
    TexFormat format = TexFormat::RGBA16F;
    bool renderTarget = true;
};

class Texture {
public:
    virtual ~Texture() = default;
    [[nodiscard]] int width() const noexcept { return desc_.width; }
    [[nodiscard]] int height() const noexcept { return desc_.height; }
    [[nodiscard]] TexFormat format() const noexcept { return desc_.format; }
    [[nodiscard]] const TextureDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] size_t bytes() const noexcept {
        return static_cast<size_t>(desc_.width) * static_cast<size_t>(desc_.height) * static_cast<size_t>(bytesPerPixel(desc_.format));
    }

protected:
    explicit Texture(const TextureDesc& d) : desc_(d) {}
    TextureDesc desc_;
};

using TexturePtr = std::shared_ptr<Texture>;

enum class Kernel : uint16_t {
    YuvToRgb,
    Composite,
    Fill,
    Copy,
    Mix,
    RgbToYuv,
    BlurGaussian,
    BlurDirectional,
    BlurRadial,
    Sharpen,
    Threshold,
    Combine,
    Vignette,
    Noise,
    Pixelate,
    RgbSplit,
    LensDistort,
    Glitch,
    Shake,
    ColorBasic,
    ColorWheels,
    Curves,
    Lut3D,
    ChromaKey,
    Mask,
    Dilate,
    TextCompose,
    Transition,
    Count
};

// File stem of the kernel source in render/kernels ("yuv_to_rgb").
const char* kernelSourceName(Kernel k);

inline constexpr int kParamSlots = 16;
inline constexpr int kMaxInputs = 4;

struct KernelParams {
    std::array<float, kParamSlots * 4> v{};
    void set(int slot, float x, float y = 0.f, float z = 0.f, float w = 0.f) {
        v[static_cast<size_t>(slot) * 4 + 0] = x;
        v[static_cast<size_t>(slot) * 4 + 1] = y;
        v[static_cast<size_t>(slot) * 4 + 2] = z;
        v[static_cast<size_t>(slot) * 4 + 3] = w;
    }
};

enum class BackendKind { CPU, D3D11, D3D12 };

struct DeviceInfo {
    BackendKind kind = BackendKind::CPU;
    std::string name;       // "Direct3D 11", "CPU"
    std::string adapter;    // GPU name
    std::string vendor;     // NVIDIA / AMD / Intel / Microsoft / CPU
    uint64_t dedicatedVideoMemory = 0;
    bool software = false;  // WARP / CPU
};

class Device {
public:
    virtual ~Device() = default;
    [[nodiscard]] virtual const DeviceInfo& info() const = 0;
    virtual TexturePtr createTexture(const TextureDesc& desc) = 0;
    // `data` layout matches the texture format (RGBA16F: IEEE half, RGBA32F: float).
    virtual void upload(Texture& tex, const void* data, int strideBytes) = 0;
    virtual void run(Kernel k, const KernelParams& params, std::initializer_list<const Texture*> inputs,
                     Texture& target) = 0;
    virtual void clear(Texture& tex, float r, float g, float b, float a) = 0;
    // Blocking read of the texture contents in its own format.
    virtual void readback(const Texture& tex, void* dst, int strideBytes) = 0;
    virtual void flush() {}
    [[nodiscard]] virtual size_t textureMemoryInUse() const = 0;
    // Native device (ID3D11Device*) for decoder interop; nullptr for CPU.
    [[nodiscard]] virtual void* nativeDevice() const { return nullptr; }
    // Imports a hardware-decoded frame (D3D11 NV12/P010 surface) as plane
    // textures without touching system memory. Returns false if unsupported.
    virtual bool importHardwareFrame(const VideoFrame& frame, TexturePtr& luma, TexturePtr& chroma) {
        (void)frame;
        (void)luma;
        (void)chroma;
        return false;
    }
    // True when the device was lost (driver reset) and must be recreated.
    [[nodiscard]] virtual bool deviceLost() const { return false; }
};

// ---- helpers --------------------------------------------------------------------

uint16_t floatToHalf(float f);
float halfToFloat(uint16_t h);

// Recycles render targets by (size, format) to avoid per-frame allocations.
class TexturePool {
public:
    explicit TexturePool(Device& device) : device_(device), state_(std::make_shared<State>()) {}
    TexturePtr acquire(int width, int height, TexFormat format = TexFormat::RGBA16F);
    void trim();                  // frees all idle textures
    [[nodiscard]] size_t idleBytes() const;
    Device& device() { return device_; }

private:
    struct State {
        std::mutex mutex;
        std::unordered_map<uint64_t, std::vector<TexturePtr>> idle;
        size_t idleBytes = 0;
    };
    Device& device_;
    std::shared_ptr<State> state_;
};

// Uploads a decoded CPU frame and converts it to premultiplied RGBA16F in the
// working colour space. Hardware frames are imported zero-copy when possible.
TexturePtr uploadVideoFrame(Device& device, TexturePool& pool, const VideoFrame& frame);

// Reads an RGBA16F / RGBA8 texture as straight RGBA8 (tests, thumbnails).
std::vector<uint8_t> readbackRgba8(Device& device, const Texture& tex, bool unpremultiply = true);

std::unique_ptr<Device> createCpuDevice(unsigned threads = 0);

}  // namespace avc::gpu
