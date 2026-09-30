// One pass of a separable Gaussian blur. t0 source.
// P(0): x,y step in uv per tap (direction / size), z sigma (taps), w radius (taps, <= 96)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float sigma = max(P(0).z, 0.001f);
    int radius = (int)min(P(0).w, 96.0f);
    float2 stepUv = xy(P(0));
    float4 sum = SAMPLE(0, ctx.uv);
    float wsum = 1.0f;
    float inv = -0.5f / (sigma * sigma);
    LOOP for (int i = 1; i <= radius; i++) {
        float w = exp(float(i * i) * inv);
        float2 o = stepUv * float(i);
        sum += (SAMPLE(0, ctx.uv + o) + SAMPLE(0, ctx.uv - o)) * w;
        wsum += 2.0f * w;
    }
    return sum / wsum;
}
