// Direct3D 11 backend. Kernels are compiled at runtime from the embedded HLSL
// (d3dcompiler_47.dll, shipped with Windows 10/11) and cached on disk.

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/file_io.h"
#include "core/log.h"
#include "core/strings.h"
#include "decode/video_frame.h"
#include "render/gpu/gpu.h"

namespace avc::gpu {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) {
            reset();
            p_ = o.p_;
            if (p_) p_->AddRef();
        }
        return *this;
    }
    ~ComPtr() { reset(); }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** put() {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void attach(T* p) {
        reset();
        p_ = p;
    }

private:
    T* p_ = nullptr;
};

DXGI_FORMAT dxgiFormat(TexFormat f) {
    switch (f) {
    case TexFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TexFormat::RGBA16: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case TexFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TexFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TexFormat::R8: return DXGI_FORMAT_R8_UNORM;
    case TexFormat::RG8: return DXGI_FORMAT_R8G8_UNORM;
    case TexFormat::R16: return DXGI_FORMAT_R16_UNORM;
    case TexFormat::RG16: return DXGI_FORMAT_R16G16_UNORM;
    }
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

class D3D11Texture final : public Texture {
public:
    D3D11Texture(const TextureDesc& d) : Texture(d) {}
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn loadCompiler() {
    static D3DCompileFn fn = [] {
        for (const wchar_t* dll : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
            if (HMODULE m = LoadLibraryW(dll)) {
                if (auto p = reinterpret_cast<D3DCompileFn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile")))) return p;
            }
        }
        return static_cast<D3DCompileFn>(nullptr);
    }();
    return fn;
}

struct CbData {
    float p[64];
    float size[4];
    float texSize[16];
};

class D3D11Device final : public Device {
public:
    bool init(const D3D11DeviceOptions& opt, std::string* error) {
        cacheDir_ = opt.shaderCacheDir;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())))) {
            if (error) *error = "CreateDXGIFactory1 failed";
            return false;
        }
        ComPtr<IDXGIAdapter1> chosen;
        if (!opt.warp) {
            ComPtr<IDXGIFactory6> f6;
            if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(f6.put())))) {
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIAdapter1> a;
                    if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                              reinterpret_cast<void**>(a.put()))))
                        break;
                    DXGI_ADAPTER_DESC1 d{};
                    a->GetDesc1(&d);
                    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                    if (!opt.adapterName.empty() && wideToUtf8(d.Description).find(opt.adapterName) == std::string::npos) continue;
                    chosen = a;
                    break;
                }
            }
            if (!chosen) {
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIAdapter1> a;
                    if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
                    DXGI_ADAPTER_DESC1 d{};
                    a->GetDesc1(&d);
                    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                    chosen = a;
                    break;
                }
            }
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (opt.debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;
        auto create = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, UINT f) {
            return D3D11CreateDevice(adapter, type, nullptr, f, levels, 4, D3D11_SDK_VERSION, device_.put(), &level_,
                                     ctx_.put());
        };
        HRESULT hr = E_FAIL;
        if (chosen) {
            hr = create(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, flags);
            if (FAILED(hr)) hr = create(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
            if (FAILED(hr) && opt.debugLayer) hr = create(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
        }
        if (FAILED(hr)) {
            info_.software = true;
            hr = create(nullptr, D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
        }
        if (FAILED(hr)) {
            if (error) *error = "D3D11CreateDevice failed (hr=0x" + hex64(static_cast<uint32_t>(hr)) + ")";
            return false;
        }
        // Decoder threads share this device.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(mt.put()))))
            mt->SetMultithreadProtected(TRUE);

        info_.kind = BackendKind::D3D11;
        info_.name = "Direct3D 11";
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDev.put())))) {
            ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(dxgiDev->GetAdapter(ad.put()))) {
                DXGI_ADAPTER_DESC d{};
                ad->GetDesc(&d);
                info_.adapter = wideToUtf8(d.Description);
                info_.dedicatedVideoMemory = d.DedicatedVideoMemory;
                switch (d.VendorId) {
                case 0x10DE: info_.vendor = "NVIDIA"; break;
                case 0x1002: case 0x1022: info_.vendor = "AMD"; break;
                case 0x8086: info_.vendor = "Intel"; break;
                case 0x1414: info_.vendor = "Microsoft"; info_.software = true; break;
                default: info_.vendor = "Unknown"; break;
                }
                ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(adapter3_.put()));
            }
        }
        shaderModel_ = level_ >= D3D_FEATURE_LEVEL_11_0 ? "5_0" : "4_0";

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CbData);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, cb_.put()))) return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device_->CreateSamplerState(&sd, linear_.put());
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        device_->CreateSamplerState(&sd, point_.put());
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        device_->CreateRasterizerState(&rd, raster_.put());

        // Vertex shader: full-screen triangle.
        const std::string vsSrc = std::string(
            "float4 VSMain(uint id : SV_VertexID) : SV_Position {"
            " float2 p = float2((id << 1) & 2, id & 2);"
            " return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }");
        std::string blob;
        if (!compile(vsSrc, "VSMain", "vs_" + shaderModel_, blob, error)) return false;
        if (FAILED(device_->CreateVertexShader(blob.data(), blob.size(), nullptr, vs_.put()))) return false;
        AVC_INFO("gpu", "Direct3D 11 device: {} ({}, feature level {:x}, {} MB)", info_.adapter, info_.vendor,
                 static_cast<unsigned>(level_), info_.dedicatedVideoMemory >> 20);
        return true;
    }

    const DeviceInfo& info() const override { return info_; }

    TexturePtr createTexture(const TextureDesc& desc) override {
        auto t = std::make_shared<D3D11Texture>(desc);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(std::max(1, desc.width));
        td.Height = static_cast<UINT>(std::max(1, desc.height));
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgiFormat(desc.format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (desc.renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
        std::lock_guard lock(mutex_);
        if (FAILED(device_->CreateTexture2D(&td, nullptr, t->tex.put()))) {
            checkDevice();
            AVC_ERROR("gpu", "CreateTexture2D {}x{} {} failed", desc.width, desc.height, texFormatName(desc.format));
            return nullptr;
        }
        device_->CreateShaderResourceView(t->tex.get(), nullptr, t->srv.put());
        if (desc.renderTarget) device_->CreateRenderTargetView(t->tex.get(), nullptr, t->rtv.put());
        const size_t bytes = t->bytes();
        memory_ += bytes;
        auto mem = &memory_;
        return TexturePtr(t.get(), [t, mem, bytes](Texture*) mutable {
            *mem -= bytes;
            t.reset();
        });
    }

    void upload(Texture& tex, const void* data, int stride) override {
        auto& t = static_cast<D3D11Texture&>(tex);
        std::lock_guard lock(mutex_);
        ctx_->UpdateSubresource(t.tex.get(), 0, nullptr, data, static_cast<UINT>(stride), 0);
    }

    void run(Kernel k, const KernelParams& params, std::initializer_list<const Texture*> inputs, Texture& target) override {
        ID3D11PixelShader* ps = pixelShader(k);
        if (!ps) return;
        auto& out = static_cast<D3D11Texture&>(target);
        if (!out.rtv) return;
        std::lock_guard lock(mutex_);
        CbData cb{};
        std::memcpy(cb.p, params.v.data(), sizeof(cb.p));
        cb.size[0] = static_cast<float>(out.width());
        cb.size[1] = static_cast<float>(out.height());
        cb.size[2] = 1.0f / cb.size[0];
        cb.size[3] = 1.0f / cb.size[1];
        ID3D11ShaderResourceView* srvs[kMaxInputs] = {};
        int i = 0;
        for (const Texture* in : inputs) {
            if (i >= kMaxInputs) break;
            if (in) {
                const auto& it = static_cast<const D3D11Texture&>(*in);
                srvs[i] = it.srv.get();
                cb.texSize[i * 4] = static_cast<float>(in->width());
                cb.texSize[i * 4 + 1] = static_cast<float>(in->height());
            }
            ++i;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &cb, sizeof(cb));
            ctx_->Unmap(cb_.get(), 0);
        }
        ID3D11RenderTargetView* rtv = out.rtv.get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(out.width()), static_cast<float>(out.height()), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(raster_.get());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.get(), nullptr, 0);
        ctx_->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[1] = {cb_.get()};
        ctx_->PSSetConstantBuffers(0, 1, cbs);
        ctx_->PSSetShaderResources(0, kMaxInputs, srvs);
        ID3D11SamplerState* samplers[2] = {linear_.get(), point_.get()};
        ctx_->PSSetSamplers(0, 2, samplers);
        ctx_->Draw(3, 0);
        // Unbind so the target can be an input of the next pass.
        ID3D11ShaderResourceView* nulls[kMaxInputs] = {};
        ctx_->PSSetShaderResources(0, kMaxInputs, nulls);
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx_->OMSetRenderTargets(1, &nullRtv, nullptr);
    }

    void clear(Texture& tex, float r, float g, float b, float a) override {
        auto& t = static_cast<D3D11Texture&>(tex);
        if (!t.rtv) return;
        const float c[4] = {r, g, b, a};
        std::lock_guard lock(mutex_);
        ctx_->ClearRenderTargetView(t.rtv.get(), c);
    }

    void readback(const Texture& tex, void* dst, int stride) override {
        const auto& t = static_cast<const D3D11Texture&>(tex);
        std::lock_guard lock(mutex_);
        D3D11_TEXTURE2D_DESC td{};
        t.tex->GetDesc(&td);
        const uint64_t key = (static_cast<uint64_t>(td.Width) << 40) | (static_cast<uint64_t>(td.Height) << 16) | td.Format;
        ComPtr<ID3D11Texture2D>& staging = staging_[key];
        if (!staging) {
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, staging.put()))) {
                checkDevice();
                return;
            }
        }
        ctx_->CopyResource(staging.get(), t.tex.get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
            checkDevice();
            return;
        }
        const size_t rowBytes = static_cast<size_t>(t.width()) * static_cast<size_t>(bytesPerPixel(t.format()));
        for (int y = 0; y < t.height(); ++y)
            std::memcpy(static_cast<uint8_t*>(dst) + static_cast<ptrdiff_t>(y) * stride,
                        static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, rowBytes);
        ctx_->Unmap(staging.get(), 0);
    }

    void flush() override {
        std::lock_guard lock(mutex_);
        ctx_->Flush();
    }

    size_t textureMemoryInUse() const override { return memory_.load(); }
    void* nativeDevice() const override { return device_.get(); }
    void* nativeView(const Texture& tex) const override { return static_cast<const D3D11Texture&>(tex).srv.get(); }
    void lock() override { mutex_.lock(); }
    void unlock() override { mutex_.unlock(); }
    bool deviceLost() const override { return lost_; }

    uint64_t videoMemoryUsage() const override {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
        if (adapter3_ && SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) return vi.CurrentUsage;
        return memory_.load();
    }
    uint64_t videoMemoryBudget() const override {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
        if (adapter3_ && SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) return vi.Budget;
        return info_.dedicatedVideoMemory;
    }

    bool importHardwareFrame(const VideoFrame& f, TexturePtr& luma, TexturePtr& chroma) override {
        if (f.hw != HwFrameKind::D3D11 || !f.hwTexture) return false;
        auto* src = static_cast<ID3D11Texture2D*>(f.hwTexture);
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        const bool p010 = sd.Format == DXGI_FORMAT_P010;
        if (sd.Format != DXGI_FORMAT_NV12 && !p010) return false;
        std::lock_guard lock(mutex_);
        // Copy the decoder array slice into a shader-readable surface (GPU->GPU).
        const uint64_t key = (static_cast<uint64_t>(sd.Width) << 32) | (static_cast<uint64_t>(sd.Height) << 1) | (p010 ? 1u : 0u);
        auto& slot = hwCopies_[key];
        if (!slot.tex) {
            D3D11_TEXTURE2D_DESC td = sd;
            td.ArraySize = 1;
            td.MipLevels = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, slot.tex.put()))) return false;
            auto makeView = [&](DXGI_FORMAT fmt, int w, int h, TexFormat tf) -> std::shared_ptr<D3D11Texture> {
                auto t = std::make_shared<D3D11Texture>(TextureDesc{w, h, tf, false});
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = fmt;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(device_->CreateShaderResourceView(slot.tex.get(), &vd, t->srv.put()))) return nullptr;
                t->tex = slot.tex;
                return t;
            };
            slot.luma = makeView(p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, static_cast<int>(sd.Width),
                                 static_cast<int>(sd.Height), p010 ? TexFormat::R16 : TexFormat::R8);
            slot.chroma = makeView(p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, static_cast<int>(sd.Width / 2),
                                   static_cast<int>(sd.Height / 2), p010 ? TexFormat::RG16 : TexFormat::RG8);
            if (!slot.luma || !slot.chroma) {
                slot = {};
                return false;
            }
        }
        ctx_->CopySubresourceRegion(slot.tex.get(), 0, 0, 0, 0, src, static_cast<UINT>(f.hwSubresource), nullptr);
        luma = slot.luma;
        chroma = slot.chroma;
        return true;
    }

private:
    void checkDevice() {
        const HRESULT r = device_->GetDeviceRemovedReason();
        if (r != S_OK && !lost_) {
            lost_ = true;
            AVC_ERROR("gpu", "Direct3D device removed (0x{:08x}); renderer will be recreated", static_cast<uint32_t>(r));
        }
    }

    bool compile(const std::string& src, const char* entry, const std::string& target, std::string& out, std::string* error) {
        const uint64_t hash = fnv1a64(src + "|" + entry + "|" + target);
        std::filesystem::path cacheFile;
        if (!cacheDir_.empty()) {
            cacheFile = pathFromUtf8(cacheDir_) / (hex64(hash) + ".cso");
            if (auto cached = readFileBytes(cacheFile); cached && !cached->empty()) {
                out = std::move(*cached);
                return true;
            }
        }
        D3DCompileFn fn = loadCompiler();
        if (!fn) {
            if (error) *error = "d3dcompiler_47.dll not available";
            return false;
        }
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = fn(src.data(), src.size(), "avicap_kernel", nullptr, nullptr, entry, target.c_str(),
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            const std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "unknown error";
            if (errors) errors->Release();
            if (code) code->Release();
            if (error) *error = msg;
            AVC_ERROR("gpu", "Shader compile failed ({}): {}", target, msg);
            return false;
        }
        out.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        code->Release();
        if (errors) errors->Release();
        if (!cacheFile.empty()) writeFileAtomic(cacheFile, out, AtomicWriteOptions{false, false});
        return true;
    }

    ID3D11PixelShader* pixelShader(Kernel k) {
        const size_t idx = static_cast<size_t>(k);
        std::lock_guard lock(shaderMutex_);
        if (ps_[idx]) return ps_[idx].get();
        if (psFailed_[idx]) return nullptr;
        std::string blob, err;
        if (!compile(kernelHlslSource(k), "PSMain", "ps_" + shaderModel_, blob, &err) ||
            FAILED(device_->CreatePixelShader(blob.data(), blob.size(), nullptr, ps_[idx].put()))) {
            psFailed_[idx] = true;
            AVC_ERROR("gpu", "Kernel '{}' unavailable: {}", kernelSourceName(k), err);
            return nullptr;
        }
        return ps_[idx].get();
    }

    DeviceInfo info_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIAdapter3> adapter3_;
    D3D_FEATURE_LEVEL level_ = D3D_FEATURE_LEVEL_11_0;
    std::string shaderModel_ = "5_0";
    std::string cacheDir_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11VertexShader> vs_;
    std::array<ComPtr<ID3D11PixelShader>, static_cast<size_t>(Kernel::Count)> ps_;
    std::array<bool, static_cast<size_t>(Kernel::Count)> psFailed_{};
    std::unordered_map<uint64_t, ComPtr<ID3D11Texture2D>> staging_;
    struct HwCopy {
        ComPtr<ID3D11Texture2D> tex;
        std::shared_ptr<D3D11Texture> luma, chroma;
    };
    std::unordered_map<uint64_t, HwCopy> hwCopies_;
    std::recursive_mutex mutex_;
    std::mutex shaderMutex_;
    std::atomic<size_t> memory_{0};
    bool lost_ = false;
};

}  // namespace

std::unique_ptr<Device> createD3D11Device(const D3D11DeviceOptions& opt, std::string* error) {
    auto dev = std::make_unique<D3D11Device>();
    if (!dev->init(opt, error)) return nullptr;
    return dev;
}

}  // namespace avc::gpu
