// Builds a styled title layer from coverage masks.
// t0 fill mask, t1 stroke mask (outer outline), t2 blurred mask (shadow), t3 blurred mask (glow)
// P(0) fill colour (straight rgba), P(1) gradient colour, P(2): x gradient on, y text top (uv), z text bottom (uv)
// P(3) stroke colour, P(4) shadow colour, P(5): x,y shadow offset (uv), z stroke on, w shadow on
// P(6) glow colour (a = strength), P(7) background colour, P(8): background rect (uv x0,y0,x1,y1)
#include "common.hlsli"

float4 over(float4 dst, float4 src) { return src + dst * (1.0f - src.w); }

float4 kernel_main(KERNEL_CTX) {
    float4 outc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 bg = P(7);
    float4 rect = P(8);
    if (bg.w > 0.0f && ctx.uv.x >= rect.x && ctx.uv.x <= rect.z && ctx.uv.y >= rect.y && ctx.uv.y <= rect.w)
        outc = float4(xyz(bg) * bg.w, bg.w);
    if (P(5).w > 0.5f) {
        float sa = SAMPLE(2, ctx.uv - xy(P(5))).w * P(4).w;
        outc = over(outc, float4(xyz(P(4)) * sa, sa));
    }
    float4 glowC = P(6);
    if (glowC.w > 0.0f) {
        float ga = saturate(SAMPLE(3, ctx.uv).w * glowC.w * 1.5f);
        outc = over(outc, float4(xyz(glowC) * ga, ga));
    }
    if (P(5).z > 0.5f) {
        float st = SAMPLE(1, ctx.uv).w * P(3).w;
        outc = over(outc, float4(xyz(P(3)) * st, st));
    }
    float fa = SAMPLE(0, ctx.uv).w;
    float4 fillC = P(0);
    if (P(2).x > 0.5f) {
        float t = saturate((ctx.uv.y - P(2).y) / max(P(2).z - P(2).y, 0.0001f));
        fillC = lerp(P(0), P(1), t);
    }
    float a = fa * fillC.w;
    outc = over(outc, float4(xyz(fillC) * a, a));
    return outc;
}
