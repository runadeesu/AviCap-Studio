// Vignette. t0 source. P(0): x amount (-1..1, negative = white), y size, z softness, w roundness
// P(1): aspect (w/h)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 c = SAMPLE_POINT(0, ctx.uv);
    float2 d = ctx.uv - float2(0.5f, 0.5f);
    float aspect = P(1).x;
    d.x = d.x * lerp(1.0f, aspect, P(0).w);
    float r = length(d) * 2.0f;
    float v = smoothstep(P(0).y, P(0).y + max(P(0).z, 0.001f), r);
    float amount = P(0).x;
    float3 rgb = xyz(c);
    if (amount >= 0.0f) rgb = rgb * (1.0f - v * amount);
    else rgb = lerp(rgb, float3(c.w, c.w, c.w), v * -amount);
    return float4(rgb, c.w);
}
