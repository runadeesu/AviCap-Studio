// Circular max filter on alpha (text stroke from fill coverage). t0 mask (alpha in .w).
// P(0): x radius px, y 1/width, z 1/height
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float r = P(0).x;
    int ir = (int)ceil(r);
    float best = SAMPLE_POINT(0, ctx.uv).w;
    LOOP for (int y = -ir; y <= ir; y++) {
        LOOP for (int x = -ir; x <= ir; x++) {
            float d = length(float2(float(x), float(y)));
            if (d > r + 0.5f) continue;
            float a = SAMPLE_POINT(0, ctx.uv + float2(float(x) * P(0).y, float(y) * P(0).z)).w;
            best = max(best, a * saturate(r + 0.5f - d));
        }
    }
    return float4(best, best, best, best);
}
