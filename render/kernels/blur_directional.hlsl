// Directional (motion) blur. t0 source.
// P(0): x,y total offset in uv (blur vector), z samples (<= 64), w centered (1) / trailing (0)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    int n = (int)clamp(P(0).z, 1.0f, 64.0f);
    float2 v = xy(P(0));
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    LOOP for (int i = 0; i < n; i++) {
        float t = n > 1 ? float(i) / float(n - 1) : 0.5f;
        if (P(0).w > 0.5f) t = t - 0.5f;
        sum += SAMPLE(0, ctx.uv + v * t);
    }
    return sum / float(n);
}
