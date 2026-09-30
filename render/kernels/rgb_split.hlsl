// RGB split / chromatic aberration. t0 source.
// P(0): x,y red offset (uv), z,w blue offset (uv)
// P(1): x radial mode (offsets scale with distance from centre), y strength
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float2 ro = xy(P(0));
    float2 bo = zw(P(0));
    if (P(1).x > 0.5f) {
        float2 d = ctx.uv - float2(0.5f, 0.5f);
        ro = d * P(1).y;
        bo = -d * P(1).y;
    }
    float4 g = SAMPLE(0, ctx.uv);
    float4 r = SAMPLE(0, ctx.uv + ro);
    float4 b = SAMPLE(0, ctx.uv + bo);
    float a = max(g.w, max(r.w, b.w));
    return float4(r.x, g.y, b.z, a);
}
