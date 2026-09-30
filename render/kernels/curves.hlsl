// Curves via lookup texture. t0 source, t1 LUT (256 x 8):
//   row 0 master (luma), 1 red, 2 green, 3 blue,
//   row 4 hue vs hue (offset), 5 hue vs saturation (gain), 6 hue vs luma (offset), 7 luma vs saturation (gain)
// Rows 0-3 store output value in .x; rows 4-7 store the adjustment centred at 0.5 in .x.
// P(0).x: strength
#include "common.hlsli"

float lutRow(float v, float row) { return SAMPLE(1, float2(saturate(v) * (255.0f / 256.0f) + 0.5f / 256.0f, (row + 0.5f) / 8.0f)).x; }

float4 kernel_main(KERNEL_CTX) {
    float4 src = SAMPLE_POINT(0, ctx.uv);
    if (src.w <= 0.0f) return src;
    float3 c0 = xyz(src) / src.w;
    float3 c = float3(lutRow(c0.x, 0.0f), lutRow(c0.y, 0.0f), lutRow(c0.z, 0.0f));
    c = float3(lutRow(c.x, 1.0f), lutRow(c.y, 2.0f), lutRow(c.z, 3.0f));
    float3 hsv = rgb2hsv(saturate(c));
    float hueShift = lutRow(hsv.x, 4.0f) - 0.5f;
    float satGain = lutRow(hsv.x, 5.0f) * 2.0f;
    float lumOff = lutRow(hsv.x, 6.0f) - 0.5f;
    float lumSat = lutRow(luma709(c), 7.0f) * 2.0f;
    hsv.x = frac(hsv.x + hueShift);
    hsv.y = saturate(hsv.y * satGain * lumSat);
    hsv.z = saturate(hsv.z + lumOff * 0.5f);
    c = hsv2rgb(hsv);
    c = lerp(c0, c, P(0).x);
    return float4(saturate(c) * src.w, src.w);
}
