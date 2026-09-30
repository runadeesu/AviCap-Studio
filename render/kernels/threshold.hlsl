// Bright-pass for bloom/glow. t0 source. P(0): x threshold, y knee, z use alpha as mask (glow)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 c = SAMPLE(0, ctx.uv);
    if (P(0).z > 0.5f) return c;
    float l = luma709(xyz(c));
    float w = smoothstep(P(0).x - P(0).y, P(0).x + P(0).y, l);
    return c * w;
}
