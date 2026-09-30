// Radial zoom / spin blur. t0 source.
// P(0): x,y centre (uv), z amount, w mode (0 zoom, 1 spin)
// P(1): x samples
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    int n = (int)clamp(P(1).x, 2.0f, 48.0f);
    float2 c = xy(P(0));
    float2 d = ctx.uv - c;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    LOOP for (int i = 0; i < n; i++) {
        float t = (float(i) / float(n - 1) - 0.5f) * P(0).z;
        float2 uv;
        if (P(0).w < 0.5f) {
            uv = c + d * (1.0f + t);
        } else {
            float s = sin(t);
            float co = cos(t);
            uv = c + float2(d.x * co - d.y * s, d.x * s + d.y * co);
        }
        sum += SAMPLE(0, uv);
    }
    return sum / float(n);
}
