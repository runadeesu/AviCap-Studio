// 3D LUT (.cube) packed into a 2D texture of (N*N) x N: x = r + b*N, y = g.
// t0 source, t1 packed LUT. P(0): x N, y strength
#include "common.hlsli"

float3 lutSample(float3 c, float n) {
    float b = saturate(c.z) * (n - 1.0f);
    float b0 = floor(b);
    float b1 = min(b0 + 1.0f, n - 1.0f);
    float fb = b - b0;
    float rx = saturate(c.x) * (n - 1.0f) + 0.5f;
    float gy = (saturate(c.y) * (n - 1.0f) + 0.5f) / n;
    float3 s0 = xyz(SAMPLE(1, float2((rx + b0 * n) / (n * n), gy)));
    float3 s1 = xyz(SAMPLE(1, float2((rx + b1 * n) / (n * n), gy)));
    return lerp(s0, s1, fb);
}

float4 kernel_main(KERNEL_CTX) {
    float4 src = SAMPLE_POINT(0, ctx.uv);
    if (src.w <= 0.0f) return src;
    float3 c = xyz(src) / src.w;
    float3 graded = lutSample(c, P(0).x);
    c = lerp(c, graded, P(0).y);
    return float4(saturate(c) * src.w, src.w);
}
