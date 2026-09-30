// Chroma key. t0 source.
// P(0): key colour (rgb), w tolerance
// P(1): x softness (edge), y spill suppression, z matte clean (clip black), w matte clip white
// P(2): x output mode (0 composite, 1 matte view)
#include "common.hlsli"

float2 chromaUV(float3 c) {
    float y = luma709(c);
    return float2((c.z - y) * 0.5389f, (c.x - y) * 0.6350f);
}

float4 kernel_main(KERNEL_CTX) {
    float4 src = SAMPLE_POINT(0, ctx.uv);
    if (src.w <= 0.0f) return src;
    float3 c = xyz(src) / src.w;
    float3 key = xyz(P(0));
    float d = distance(chromaUV(c), chromaUV(key));
    float tol = P(0).w * 0.5f;
    float soft = max(P(1).x * 0.5f, 0.0001f);
    float alpha = smoothstep(tol, tol + soft, d);
    alpha = saturate((alpha - P(1).z) / max(P(1).w - P(1).z, 0.001f));
    // Spill suppression: limit the key's dominant channel.
    float spill = P(1).y;
    if (spill > 0.0f) {
        if (key.y >= key.x && key.y >= key.z) {
            float lim = max(c.x, c.z);
            c.y = lerp(c.y, min(c.y, lim), spill);
        } else if (key.z >= key.x) {
            float lim = max(c.x, c.y);
            c.z = lerp(c.z, min(c.z, lim), spill);
        } else {
            float lim = max(c.y, c.z);
            c.x = lerp(c.x, min(c.x, lim), spill);
        }
    }
    float a = alpha * src.w;
    if (P(2).x > 0.5f) return float4(a, a, a, 1.0f);
    return float4(c * a, a);
}
