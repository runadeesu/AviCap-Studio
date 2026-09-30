// Composites a transformed layer over the accumulated frame.
// t0: accumulated frame (premultiplied), t1: layer (premultiplied)
// P(0): inverse affine row 0 (a, b, c): layerPx.x = a*x + b*y + c (x,y output px)
// P(1): inverse affine row 1
// P(2): layer size (w, h, 1/w, 1/h)
// P(3): crop rectangle in layer px (x0, y0, x1, y1)
// P(4): x opacity, y blend mode, z output px per layer px (edge AA), w has accumulated input
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 dst = P(4).w > 0.5f ? SAMPLE_POINT(0, ctx.uv) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 lp = float2(dot(xyz(P(0)), float3(ctx.pos, 1.0f)), dot(xyz(P(1)), float3(ctx.pos, 1.0f)));
    float4 crop = P(3);
    float edge = min(min(lp.x - crop.x, crop.z - lp.x), min(lp.y - crop.y, crop.w - lp.y));
    float coverage = saturate(edge * max(P(4).z, 0.0001f) + 0.5f);
    if (coverage <= 0.0f) return dst;
    float2 luv = float2(lp.x * P(2).z, lp.y * P(2).w);
    float4 src = SAMPLE(1, luv) * (coverage * P(4).x);
    return blendOver(dst, src, (int)P(4).y);
}
