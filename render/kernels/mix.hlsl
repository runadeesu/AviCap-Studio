// Linear mix of two images. t0 a, t1 b. P(0).x amount of b.
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) { return lerp(SAMPLE_POINT(0, ctx.uv), SAMPLE_POINT(1, ctx.uv), P(0).x); }
