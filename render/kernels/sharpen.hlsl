// Unsharp mask. t0 original, t1 blurred. P(0): x amount, y threshold
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 o = SAMPLE_POINT(0, ctx.uv);
    float4 b = SAMPLE_POINT(1, ctx.uv);
    float3 detail = xyz(o) - xyz(b);
    float mag = abs(luma709(detail));
    float gate = smoothstep(P(0).y, P(0).y + 0.02f, mag);
    float3 c = xyz(o) + detail * (P(0).x * gate);
    return float4(clamp(c, float3(0.0f, 0.0f, 0.0f), float3(o.w, o.w, o.w)), o.w);
}
