// Digital glitch: displaced blocks with channel shift and scanlines. t0 source.
// P(0): x amount, y seed (changes per frame), z block height (uv), w colour shift (uv)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float amount = P(0).x;
    float seed = floor(P(0).y);
    float bh = max(P(0).z, 0.002f);
    float row = floor(ctx.uv.y / bh);
    float rnd = hash12(float2(row, seed));
    float2 uv = ctx.uv;
    if (rnd < amount * 0.6f) uv.x = uv.x + (hash12(float2(seed, row)) - 0.5f) * 0.2f * amount;
    float shift = P(0).w * amount;
    float4 g = SAMPLE(0, uv);
    float4 r = SAMPLE(0, uv + float2(shift, 0.0f));
    float4 b = SAMPLE(0, uv - float2(shift, 0.0f));
    float scan = 1.0f - 0.15f * amount * step(0.5f, frac(ctx.pos.y * 0.5f));
    return float4(r.x * scan, g.y * scan, b.z * scan, max(g.w, max(r.w, b.w)));
}
