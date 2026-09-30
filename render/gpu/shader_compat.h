#pragma once
// C++ implementation of the HLSL subset used by AviCap kernels.
//
// Kernels in render/kernels/*.hlsl are compiled twice: by D3DCompile for the
// GPU backends and as C++ (with this header) for the CPU backend. Kernel code
// therefore follows a few rules:
//   * vectors: float2/float3/float4 with .x .y .z .w members only (no swizzles;
//     use xy(v), xyz(v) helpers), explicit constructors
//   * textures only through SAMPLE(i, uv) / SAMPLE_POINT(i, uv) / TEXSIZE(i)
//   * parameters only through P(n) (float4) and ctx fields
//   * loops use LOOP before `for` when the bound is dynamic

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace avc::hlsl {

struct float2 {
    float x = 0, y = 0;
    constexpr float2() = default;
    constexpr explicit float2(float s) : x(s), y(s) {}
    constexpr float2(float a, float b) : x(a), y(b) {}
};

struct float3 {
    float x = 0, y = 0, z = 0;
    constexpr float3() = default;
    constexpr explicit float3(float s) : x(s), y(s), z(s) {}
    constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
    constexpr float3(float2 v, float c) : x(v.x), y(v.y), z(c) {}
};

struct float4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr float4() = default;
    constexpr explicit float4(float s) : x(s), y(s), z(s), w(s) {}
    constexpr float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    constexpr float4(float3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    constexpr float4(float2 a, float2 b) : x(a.x), y(a.y), z(b.x), w(b.y) {}
    constexpr float4(float2 a, float c, float d) : x(a.x), y(a.y), z(c), w(d) {}
};

// ---- component-wise operators ------------------------------------------------
#define AVC_VEC_OPS(T, N)                                                                          \
    inline T operator+(T a, T b) { return apply2(a, b, [](float p, float q) { return p + q; }); } \
    inline T operator-(T a, T b) { return apply2(a, b, [](float p, float q) { return p - q; }); } \
    inline T operator*(T a, T b) { return apply2(a, b, [](float p, float q) { return p * q; }); } \
    inline T operator/(T a, T b) { return apply2(a, b, [](float p, float q) { return p / q; }); } \
    inline T operator+(T a, float s) { return a + T(s); }                                         \
    inline T operator-(T a, float s) { return a - T(s); }                                         \
    inline T operator*(T a, float s) { return a * T(s); }                                         \
    inline T operator/(T a, float s) { return a / T(s); }                                         \
    inline T operator+(float s, T a) { return T(s) + a; }                                         \
    inline T operator-(float s, T a) { return T(s) - a; }                                         \
    inline T operator*(float s, T a) { return T(s) * a; }                                         \
    inline T operator/(float s, T a) { return T(s) / a; }                                         \
    inline T operator-(T a) { return apply1(a, [](float p) { return -p; }); }                     \
    inline T& operator+=(T& a, T b) { return a = a + b; }                                         \
    inline T& operator-=(T& a, T b) { return a = a - b; }                                         \
    inline T& operator*=(T& a, T b) { return a = a * b; }                                         \
    inline T& operator/=(T& a, T b) { return a = a / b; }                                         \
    inline T& operator+=(T& a, float s) { return a = a + s; }                                     \
    inline T& operator-=(T& a, float s) { return a = a - s; }                                     \
    inline T& operator*=(T& a, float s) { return a = a * s; }                                     \
    inline T& operator/=(T& a, float s) { return a = a / s; }

template <typename F>
inline float2 apply1(float2 a, F f) { return {f(a.x), f(a.y)}; }
template <typename F>
inline float3 apply1(float3 a, F f) { return {f(a.x), f(a.y), f(a.z)}; }
template <typename F>
inline float4 apply1(float4 a, F f) { return {f(a.x), f(a.y), f(a.z), f(a.w)}; }
template <typename F>
inline float2 apply2(float2 a, float2 b, F f) { return {f(a.x, b.x), f(a.y, b.y)}; }
template <typename F>
inline float3 apply2(float3 a, float3 b, F f) { return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z)}; }
template <typename F>
inline float4 apply2(float4 a, float4 b, F f) { return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w)}; }
template <typename F>
inline float2 apply3(float2 a, float2 b, float2 c, F f) { return {f(a.x, b.x, c.x), f(a.y, b.y, c.y)}; }
template <typename F>
inline float3 apply3(float3 a, float3 b, float3 c, F f) { return {f(a.x, b.x, c.x), f(a.y, b.y, c.y), f(a.z, b.z, c.z)}; }
template <typename F>
inline float4 apply3(float4 a, float4 b, float4 c, F f) {
    return {f(a.x, b.x, c.x), f(a.y, b.y, c.y), f(a.z, b.z, c.z), f(a.w, b.w, c.w)};
}

AVC_VEC_OPS(float2, 2)
AVC_VEC_OPS(float3, 3)
AVC_VEC_OPS(float4, 4)
#undef AVC_VEC_OPS

// ---- swizzle helpers (same names exist in the HLSL prelude) -------------------
inline float2 xy(float4 v) { return {v.x, v.y}; }
inline float2 zw(float4 v) { return {v.z, v.w}; }
inline float2 xy(float3 v) { return {v.x, v.y}; }
inline float3 xyz(float4 v) { return {v.x, v.y, v.z}; }
inline float2 yx(float2 v) { return {v.y, v.x}; }

// ---- scalar intrinsics ---------------------------------------------------------
inline float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float frac(float v) { return v - std::floor(v); }
inline float step(float edge, float x) { return x >= edge ? 1.0f : 0.0f; }
inline float smoothstep(float a, float b, float x) {
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
inline float sign(float v) { return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); }
inline float rsqrt(float v) { return 1.0f / std::sqrt(v); }
inline float clamp(float v, float a, float b) { return std::clamp(v, a, b); }
using std::atan;
using std::atan2;
using std::ceil;
using std::cos;
using std::exp;
using std::exp2;
using std::floor;
using std::fmod;
using std::log;
using std::log2;
using std::pow;
using std::sin;
using std::sqrt;
using std::tan;
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float abs(float v) { return std::fabs(v); }

// ---- vector intrinsics -----------------------------------------------------------
#define AVC_VEC_FN1(name, expr)                                        \
    inline float2 name(float2 v) { return apply1(v, [](float p) { return expr; }); } \
    inline float3 name(float3 v) { return apply1(v, [](float p) { return expr; }); } \
    inline float4 name(float4 v) { return apply1(v, [](float p) { return expr; }); }
AVC_VEC_FN1(saturate, std::clamp(p, 0.0f, 1.0f))
AVC_VEC_FN1(abs, std::fabs(p))
AVC_VEC_FN1(floor, std::floor(p))
AVC_VEC_FN1(ceil, std::ceil(p))
AVC_VEC_FN1(frac, p - std::floor(p))
AVC_VEC_FN1(sqrt, std::sqrt(p))
AVC_VEC_FN1(exp, std::exp(p))
AVC_VEC_FN1(exp2, std::exp2(p))
AVC_VEC_FN1(log2, std::log2(p))
AVC_VEC_FN1(sin, std::sin(p))
AVC_VEC_FN1(cos, std::cos(p))
AVC_VEC_FN1(sign, (p > 0 ? 1.0f : (p < 0 ? -1.0f : 0.0f)))
#undef AVC_VEC_FN1

#define AVC_VEC_FN2(name, expr)                                                                     \
    inline float2 name(float2 a, float2 b) { return apply2(a, b, [](float p, float q) { return expr; }); } \
    inline float3 name(float3 a, float3 b) { return apply2(a, b, [](float p, float q) { return expr; }); } \
    inline float4 name(float4 a, float4 b) { return apply2(a, b, [](float p, float q) { return expr; }); }
AVC_VEC_FN2(min, (p < q ? p : q))
AVC_VEC_FN2(max, (p > q ? p : q))
AVC_VEC_FN2(pow, std::pow(p, q))
AVC_VEC_FN2(step, (q >= p ? 1.0f : 0.0f))
AVC_VEC_FN2(fmod, std::fmod(p, q))
#undef AVC_VEC_FN2

template <typename V>
inline V lerp(V a, V b, float t) { return a + (b - a) * t; }
inline float2 lerp(float2 a, float2 b, float2 t) { return a + (b - a) * t; }
inline float3 lerp(float3 a, float3 b, float3 t) { return a + (b - a) * t; }
inline float4 lerp(float4 a, float4 b, float4 t) { return a + (b - a) * t; }
inline float2 clamp(float2 v, float2 a, float2 b) { return min(max(v, a), b); }
inline float3 clamp(float3 v, float3 a, float3 b) { return min(max(v, a), b); }
inline float4 clamp(float4 v, float4 a, float4 b) { return min(max(v, a), b); }
inline float2 clamp(float2 v, float a, float b) { return clamp(v, float2(a), float2(b)); }
inline float3 clamp(float3 v, float a, float b) { return clamp(v, float3(a), float3(b)); }
inline float4 clamp(float4 v, float a, float b) { return clamp(v, float4(a), float4(b)); }
inline float3 smoothstep(float a, float b, float3 x) {
    return {smoothstep(a, b, x.x), smoothstep(a, b, x.y), smoothstep(a, b, x.z)};
}

inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float length(float2 v) { return std::sqrt(dot(v, v)); }
inline float length(float3 v) { return std::sqrt(dot(v, v)); }
inline float distance(float2 a, float2 b) { return length(a - b); }
inline float2 normalize(float2 v) { return v * (1.0f / std::max(1e-20f, length(v))); }
inline float3 normalize(float3 v) { return v * (1.0f / std::max(1e-20f, length(v))); }
inline float3 cross(float3 a, float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// ---- textures ---------------------------------------------------------------------

// Read-only float4 image view used by CPU kernels (clamp-to-edge addressing).
struct Texture {
    const float4* data = nullptr;
    int width = 0;
    int height = 0;

    float4 load(int x, int y) const {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }
    float4 samplePoint(float2 uv) const {
        if (!data) return float4(0.0f);
        return load(static_cast<int>(std::floor(uv.x * width)), static_cast<int>(std::floor(uv.y * height)));
    }
    float4 sample(float2 uv) const {
        if (!data) return float4(0.0f);
        const float fx = uv.x * width - 0.5f, fy = uv.y * height - 0.5f;
        const float x0f = std::floor(fx), y0f = std::floor(fy);
        const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
        const float tx = fx - x0f, ty = fy - y0f;
        const float4 a = load(x0, y0), b = load(x0 + 1, y0), c = load(x0, y0 + 1), d = load(x0 + 1, y0 + 1);
        return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
    }
};

struct KernelContext {
    float2 uv;    // normalized position of the pixel centre
    float2 pos;   // pixel centre in output pixels (x + 0.5, y + 0.5)
    float2 size;  // output size in pixels
    const Texture* tex[4] = {};
    const float4* params = nullptr;
};

}  // namespace avc::hlsl

namespace avc::hlsl {
// Context of the kernel invocation running on this thread; lets helper
// functions sample textures like global HLSL resources do.
#if defined(__GNUC__) && !defined(_WIN32)
extern thread_local const KernelContext* tls_ctx __attribute__((tls_model("initial-exec")));
#else
extern thread_local const KernelContext* tls_ctx;
#endif
}  // namespace avc::hlsl

// Kernel-facing macros (CPU flavour). The HLSL prelude defines the same names.
#define KERNEL_CTX const ::avc::hlsl::KernelContext& ctx
#define P(n) (::avc::hlsl::tls_ctx->params[n])
#define SAMPLE(i, uvv) (::avc::hlsl::tls_ctx->tex[i]->sample(uvv))
#define SAMPLE_POINT(i, uvv) (::avc::hlsl::tls_ctx->tex[i]->samplePoint(uvv))
#define TEXSIZE(i) (::avc::hlsl::float2(static_cast<float>(::avc::hlsl::tls_ctx->tex[i]->width), static_cast<float>(::avc::hlsl::tls_ctx->tex[i]->height)))
#define LOOP
#define UNROLL
#define STATIC_CONST static const
