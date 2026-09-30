// Pixelate / mosaic. t0 source.
// P(0): x,y cell size in uv, z region mode (0 whole, 1 rectangle, 2 ellipse), w feather (uv)
// P(1): region centre (x,y) and half size (z,w) in uv
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float2 cell = max(xy(P(0)), float2(0.0001f, 0.0001f));
    float2 cuv = (floor(ctx.uv / cell) + 0.5f) * cell;
    float4 pix = SAMPLE(0, cuv);
    int mode = (int)P(0).z;
    if (mode == 0) return pix;
    float4 orig = SAMPLE_POINT(0, ctx.uv);
    float2 d = (ctx.uv - xy(P(1))) / max(zw(P(1)), float2(0.0001f, 0.0001f));
    float dist;
    if (mode == 1) dist = max(abs(d.x), abs(d.y));
    else dist = length(d);
    float feather = max(P(0).w, 0.0001f);
    float m = 1.0f - smoothstep(1.0f - feather, 1.0f, dist);
    return lerp(orig, pix, m);
}
