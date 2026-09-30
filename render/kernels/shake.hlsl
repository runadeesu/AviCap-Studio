// Camera shake / reposition inside the layer. t0 source.
// P(0): x,y offset (uv), z rotation (radians), w zoom
// P(1): x aspect (w/h)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float aspect = P(1).x;
    float2 d = ctx.uv - float2(0.5f, 0.5f);
    d.x = d.x * aspect;
    float s = sin(-P(0).z);
    float c = cos(-P(0).z);
    d = float2(d.x * c - d.y * s, d.x * s + d.y * c) / max(P(0).w, 0.01f);
    d.x = d.x / aspect;
    return SAMPLE(0, d + float2(0.5f, 0.5f) - xy(P(0)));
}
