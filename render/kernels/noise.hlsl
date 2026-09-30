// Noise / film grain. t0 source.
// P(0): x amount, y grain size (px), z monochrome, w seed
// P(1): x film grain response (1: stronger in midtones), y output height px
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 c = SAMPLE_POINT(0, ctx.uv);
    float2 p = ctx.pos / max(P(0).y, 0.5f) + float2(P(0).w * 13.1f, P(0).w * 7.7f);
    float n1 = valueNoise(p) - 0.5f;
    float n2 = valueNoise(p + float2(17.3f, 3.1f)) - 0.5f;
    float n3 = valueNoise(p + float2(5.9f, 29.4f)) - 0.5f;
    float3 n = P(0).z > 0.5f ? float3(n1, n1, n1) : float3(n1, n2, n3);
    float3 rgb = xyz(c);
    float resp = 1.0f;
    if (P(1).x > 0.5f) {
        float l = luma709(rgb / max(c.w, 0.0001f));
        resp = 4.0f * l * (1.0f - l) + 0.15f;
    }
    rgb = rgb + n * (P(0).x * resp * c.w);
    return float4(clamp(rgb, float3(0.0f, 0.0f, 0.0f), float3(c.w, c.w, c.w)), c.w);
}
