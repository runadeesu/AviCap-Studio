// Shape / texture mask applied to alpha. t0 source, t1 mask texture (mode 2).
// P(0): x,y centre (uv), z,w half size (uv)
// P(1): x rotation (radians), y feather (uv), z expansion (uv), w invert
// P(2): x opacity, y mode (0 rectangle, 1 ellipse, 2 texture), z aspect (w/h)
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 src = SAMPLE_POINT(0, ctx.uv);
    int mode = (int)P(2).y;
    float m;
    if (mode == 2) {
        m = SAMPLE(1, ctx.uv).x;
    } else {
        float aspect = P(2).z;
        float2 d = ctx.uv - xy(P(0));
        d.x = d.x * aspect;
        float s = sin(-P(1).x);
        float c = cos(-P(1).x);
        d = float2(d.x * c - d.y * s, d.x * s + d.y * c);
        d.x = d.x / aspect;
        float2 hs = zw(P(0)) + float2(P(1).z, P(1).z);
        hs = max(hs, float2(0.0001f, 0.0001f));
        float feather = max(P(1).y, 0.0001f);
        float dist;
        if (mode == 0) {
            float2 q = abs(d) - hs;
            dist = max(q.x, q.y);
        } else {
            dist = (length(d / hs) - 1.0f) * min(hs.x, hs.y);
        }
        m = 1.0f - smoothstep(-feather * 0.5f, feather * 0.5f, dist);
    }
    if (P(1).w > 0.5f) m = 1.0f - m;
    m = lerp(1.0f, m, P(2).x);
    return src * m;
}
