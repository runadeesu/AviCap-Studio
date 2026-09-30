#include <string>
#include <string_view>

#include "render/gpu/gpu.h"

namespace avc::gpu::embedded {
std::string_view find(std::string_view name);
std::size_t count();
std::string_view nameAt(std::size_t i);
}  // namespace avc::gpu::embedded

namespace avc::gpu {

// HLSL prelude for GPU compilation: maps the kernel macros to D3D resources.
const char* hlslPrelude() {
    return R"(
Texture2D<float4> g_tex0 : register(t0);
Texture2D<float4> g_tex1 : register(t1);
Texture2D<float4> g_tex2 : register(t2);
Texture2D<float4> g_tex3 : register(t3);
SamplerState g_linear : register(s0);
SamplerState g_point : register(s1);
cbuffer KernelParams : register(b0) {
    float4 g_p[16];
    float4 g_size;      // xy output size, zw 1/size
    float4 g_texSize[4];
};
struct KernelContext { float2 uv; float2 pos; float2 size; };
#define KERNEL_CTX KernelContext ctx
#define P(n) g_p[n]
#define SAMPLE(i, uvv) g_tex##i.SampleLevel(g_linear, uvv, 0)
#define SAMPLE_POINT(i, uvv) g_tex##i.SampleLevel(g_point, uvv, 0)
#define TEXSIZE(i) g_texSize[i].xy
#define LOOP [loop]
#define UNROLL [unroll]
#define STATIC_CONST static const
#define xy(v) ((v).xy)
#define zw(v) ((v).zw)
#define xyz(v) ((v).xyz)
#define yx(v) ((v).yx)
)";
}

const char* hlslEpilogue() {
    return R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 svpos : SV_Position) : SV_Target {
    KernelContext ctx;
    ctx.pos = svpos.xy;
    ctx.size = g_size.xy;
    ctx.uv = svpos.xy * g_size.zw;
    return kernel_main(ctx);
}
)";
}

// Full HLSL source for a kernel: prelude + kernel (with includes resolved) + epilogue.
std::string kernelHlslSource(Kernel k) {
    const std::string file = std::string(kernelSourceName(k)) + ".hlsl";
    std::string body(embedded::find(file));
    // Resolve #include "x.hlsli" from the embedded set (one level is enough).
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string_view line(body.data() + pos, eol - pos);
        if (line.rfind("#include \"", 0) == 0) {
            const size_t q = line.find('"', 10);
            const std::string inc(line.substr(10, q - 10));
            out += std::string(embedded::find(inc));
            out += '\n';
        } else {
            out.append(line);
            out += '\n';
        }
        pos = eol + 1;
    }
    return std::string(hlslPrelude()) + out + hlslEpilogue();
}

}  // namespace avc::gpu
