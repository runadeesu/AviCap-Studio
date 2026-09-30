// Lift / gamma / gain / offset + shadows / midtones / highlights wheels. t0 source.
// Wheels are RGB offsets (w = master) : P(0) lift, P(1) gamma, P(2) gain, P(3) offset,
// P(4) shadows, P(5) midtones, P(6) highlights
#include "common.hlsli"

float3 wheel(float4 w) { return float3(w.x + w.w, w.y + w.w, w.z + w.w); }

float4 kernel_main(KERNEL_CTX) {
    float4 src = SAMPLE_POINT(0, ctx.uv);
    if (src.w <= 0.0f) return src;
    float3 c = xyz(src) / src.w;
    float3 lift = wheel(P(0));
    float3 gamma = wheel(P(1));
    float3 gain = wheel(P(2));
    float3 offs = wheel(P(3));
    // ASC-CDL style: out = (in * gain + lift * (1 - in)) ^ (1 / gamma) + offset
    c = c * (float3(1.0f, 1.0f, 1.0f) + gain) + lift * (float3(1.0f, 1.0f, 1.0f) - c);
    float3 g = max(float3(1.0f, 1.0f, 1.0f) + gamma, float3(0.05f, 0.05f, 0.05f));
    c = pow(max(c, float3(0.0f, 0.0f, 0.0f)), float3(1.0f, 1.0f, 1.0f) / g) + offs;
    float l = luma709(c);
    float ws = 1.0f - smoothstep(0.0f, 0.45f, l);
    float wh = smoothstep(0.55f, 1.0f, l);
    float wm = saturate(1.0f - ws - wh);
    c = c + wheel(P(4)) * ws + wheel(P(5)) * wm + wheel(P(6)) * wh;
    c = saturate(c);
    return float4(c * src.w, src.w);
}
