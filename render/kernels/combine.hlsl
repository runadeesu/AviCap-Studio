// Adds a (blurred) light layer onto a base. t0 base, t1 light.
// P(0): x mode (0 add, 1 screen, 2 glow-behind), y intensity
// P(1): tint colour (rgb), w tint amount
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 b = SAMPLE_POINT(0, ctx.uv);
    float4 l = SAMPLE(1, ctx.uv) * P(0).y;
    float3 tinted = lerp(xyz(l), float3(P(1).x, P(1).y, P(1).z) * l.w, P(1).w);
    l = float4(tinted, l.w);
    int mode = (int)P(0).x;
    if (mode == 1) {
        float3 c = xyz(b) + xyz(l) - xyz(b) * xyz(l);
        return float4(c, max(b.w, saturate(l.w)));
    }
    if (mode == 2) {
        // Glow placed behind the base (keeps the original on top).
        float4 glow = float4(min(xyz(l), float3(1.0f, 1.0f, 1.0f)), saturate(l.w));
        return b + glow * (1.0f - b.w);
    }
    return float4(min(xyz(b) + xyz(l), float3(1.0f, 1.0f, 1.0f)), max(b.w, saturate(l.w)));
}
