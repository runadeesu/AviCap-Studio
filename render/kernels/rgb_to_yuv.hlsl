// Export conversion of the final frame to one YUV plane (8-bit targets).
// t0: frame (premultiplied, composited over opaque background)
// P(0): x plane (0 Y, 1 U, 2 V, 3 interleaved UV), y full range, z output scale
//       (1 for 8-bit / MSB-aligned targets, 1023/65535 for 10-bit LSB-aligned R16)
// P(1): Kr, Kg, Kb
#include "common.hlsli"

float4 kernel_main(KERNEL_CTX) {
    float4 c = SAMPLE(0, ctx.uv);
    float3 rgb = saturate(xyz(c));
    float kr = P(1).x;
    float kg = P(1).y;
    float kb = P(1).z;
    float y = kr * rgb.x + kg * rgb.y + kb * rgb.z;
    float u = (rgb.z - y) / (2.0f * (1.0f - kb));
    float v = (rgb.x - y) / (2.0f * (1.0f - kr));
    if (P(0).y < 0.5f) {
        y = y * (219.0f / 255.0f) + 16.0f / 255.0f;
        u = u * (224.0f / 255.0f) + 128.0f / 255.0f;
        v = v * (224.0f / 255.0f) + 128.0f / 255.0f;
    } else {
        u = u + 0.5f;
        v = v + 0.5f;
    }
    float sc = P(0).z > 0.0f ? P(0).z : 1.0f;
    y = y * sc;
    u = u * sc;
    v = v * sc;
    int plane = (int)P(0).x;
    if (plane == 0) return float4(y, y, y, 1.0f);
    if (plane == 1) return float4(u, u, u, 1.0f);
    if (plane == 2) return float4(v, v, v, 1.0f);
    return float4(u, v, 0.0f, 1.0f);
}
