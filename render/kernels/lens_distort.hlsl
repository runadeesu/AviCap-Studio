// Lens distortion / fisheye. t0 source.
// P(0): x k1 (barrel > 0 / pincushion < 0), y k2, z zoom, w mode (0 polynomial, 1 fisheye)
// P(1): x aspect (w/h), y fisheye strength
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float aspect = P(1).x;
    float2 d = ctx.uv - float2(0.5f, 0.5f);
    d.x = d.x * aspect;
    float r = length(d);
    float2 src;
    if (P(0).w < 0.5f) {
        float r2 = r * r;
        float f = 1.0f + P(0).x * r2 + P(0).y * r2 * r2;
        src = d * f / max(P(0).z, 0.01f);
    } else {
        float strength = max(P(1).y, 0.001f);
        float theta = atanPoly(r * strength) / strength;
        src = r > 0.0f ? d * (theta / r) / max(P(0).z, 0.01f) : d;
    }
    src.x = src.x / aspect;
    return SAMPLE_BORDER(0, src + float2(0.5f, 0.5f));
}
