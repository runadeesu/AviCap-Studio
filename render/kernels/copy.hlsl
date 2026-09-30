// Resampling copy. t0 source. P(0): uv scale (x,y) and offset (z,w). P(1).x: transparent outside
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float2 uv = float2(ctx.uv.x * P(0).x + P(0).z, ctx.uv.y * P(0).y + P(0).w);
    if (P(1).x > 0.5f) return SAMPLE_BORDER(0, uv);
    return SAMPLE(0, uv);
}
