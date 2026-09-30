// Primary colour correction (display-referred). t0 source.
// P(0): x exposure (stops), y contrast (-1..1), z saturation (0..2), w vibrance (-1..1)
// P(1): x highlights, y shadows, z whites, w blacks (-1..1 each)
// P(2): x temperature (-1..1), y tint (-1..1), z pivot
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 src = SAMPLE_POINT(0, ctx.uv);
    if (src.w <= 0.0f) return src;
    float3 c = xyz(src) / src.w;
    // White balance (simple RGB gains).
    float temp = P(2).x;
    float tint = P(2).y;
    c = c * float3(1.0f + 0.2f * temp - 0.05f * tint, 1.0f + 0.1f * tint, 1.0f - 0.2f * temp - 0.05f * tint);
    // Exposure in (approximate) linear light.
    float3 lin = pow(max(c, float3(0.0f, 0.0f, 0.0f)), float3(2.2f, 2.2f, 2.2f)) * exp2(P(0).x);
    c = pow(lin, float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));
    // Tonal ranges.
    float l = luma709(c);
    float wHi = smoothstep(0.5f, 1.0f, l);
    float wSh = 1.0f - smoothstep(0.0f, 0.5f, l);
    c = c + P(1).x * 0.25f * wHi + P(1).y * 0.25f * wSh;
    // Whites / blacks: move the end points.
    float blacks = -P(1).w * 0.1f;
    float whites = 1.0f + P(1).z * 0.15f;
    c = (c - blacks) / max(whites - blacks, 0.05f);
    // Contrast around the pivot.
    float pivot = P(2).z;
    c = (c - pivot) * (1.0f + P(0).y) + pivot;
    // Saturation and vibrance.
    l = luma709(c);
    float3 grey = float3(l, l, l);
    c = lerp(grey, c, P(0).z);
    float mx = max(c.x, max(c.y, c.z));
    float mn = min(c.x, min(c.y, c.z));
    float satNow = mx - mn;
    float vib = P(0).w * (1.0f - saturate(satNow * 2.0f));
    c = lerp(grey, c, 1.0f + vib);
    c = saturate(c);
    return float4(c * src.w, src.w);
}
