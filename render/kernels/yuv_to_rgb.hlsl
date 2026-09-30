// Converts decoded planes into premultiplied RGBA (working space).
// t0: Y or packed RGBA/BGRA, t1: U or interleaved UV, t2: V, t3: alpha plane
// P(0): x mode (0 packed RGBA, 1 planar YUV, 2 semi-planar YUV, 3 gray, 4 packed BGRA)
//       y full range, z sample scale (16-bit containers), w has alpha plane
// P(1): YUV->RGB coefficients rv, gu, gv, bu
// P(2): x premultiply (input is straight alpha), y transfer (0 SDR, 1 PQ, 2 HLG), z BT.2020 gamut
// P(3): x uv scale x, y uv scale y (for padded textures)
#include "common.hlsli"

float pqToLinearNits(float v) {
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float p = pow(max(v, 0.0f), 1.0f / m2);
    return 10000.0f * pow(max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

float hlgToLinear(float v) {
    const float a = 0.17883277f;
    const float b = 0.28466892f;
    const float c = 0.55991073f;
    return v <= 0.5f ? (v * v) / 3.0f : (exp((v - c) / a) + b) / 12.0f;
}

float3 bt2020To709(float3 c) {
    return float3(dot(c, float3(1.6605f, -0.5876f, -0.0728f)),
                  dot(c, float3(-0.1246f, 1.1329f, -0.0083f)),
                  dot(c, float3(-0.0182f, -0.1006f, 1.1187f)));
}

float3 toneMapToSdr(float3 linearNits) {
    // Scene referred reference white at 203 nits, extended Reinhard to 1000 nits peak.
    float3 x = linearNits / 203.0f;
    const float white = 1000.0f / 203.0f;
    float3 mapped = x * (1.0f + x / (white * white)) / (1.0f + x);
    return pow(saturate(mapped), float3(1.0f / 2.4f, 1.0f / 2.4f, 1.0f / 2.4f));
}

float4 kernel_main(KERNEL_CTX) {
    float2 uv = float2(ctx.uv.x * P(3).x, ctx.uv.y * P(3).y);
    int mode = (int)P(0).x;
    float scale = P(0).z;
    float4 rgba;
    if (mode == 0) {
        rgba = SAMPLE(0, uv);
    } else if (mode == 4) {
        float4 bgra = SAMPLE(0, uv);
        rgba = float4(bgra.z, bgra.y, bgra.x, bgra.w);
    } else if (mode == 3) {
        float g = SAMPLE(0, uv).x * scale;
        rgba = float4(g, g, g, 1.0f);
    } else {
        float y = SAMPLE(0, uv).x * scale;
        float u;
        float v;
        if (mode == 2) {
            float4 c = SAMPLE(1, uv);
            u = c.x * scale;
            v = c.y * scale;
        } else {
            u = SAMPLE(1, uv).x * scale;
            v = SAMPLE(2, uv).x * scale;
        }
        if (P(0).y < 0.5f) {
            y = (y - 16.0f / 255.0f) * (255.0f / 219.0f);
            u = (u - 128.0f / 255.0f) * (255.0f / 224.0f);
            v = (v - 128.0f / 255.0f) * (255.0f / 224.0f);
        } else {
            u = u - 0.5f;
            v = v - 0.5f;
        }
        float4 k = P(1);
        float a = P(0).w > 0.5f ? SAMPLE(3, uv).x : 1.0f;
        rgba = float4(y + k.x * v, y + k.y * u + k.z * v, y + k.w * u, a);
    }
    float3 c = xyz(rgba);
    int transfer = (int)P(2).y;
    if (transfer == 1) {
        float3 nits = float3(pqToLinearNits(saturate(c.x)), pqToLinearNits(saturate(c.y)), pqToLinearNits(saturate(c.z)));
        if (P(2).z > 0.5f) nits = max(bt2020To709(nits), float3(0.0f, 0.0f, 0.0f));
        c = toneMapToSdr(nits);
    } else if (transfer == 2) {
        float3 lin = float3(hlgToLinear(saturate(c.x)), hlgToLinear(saturate(c.y)), hlgToLinear(saturate(c.z)));
        if (P(2).z > 0.5f) lin = max(bt2020To709(lin), float3(0.0f, 0.0f, 0.0f));
        c = toneMapToSdr(lin * 1000.0f);
    } else if (P(2).z > 0.5f) {
        // SDR BT.2020 content: convert gamut in (approximately) linear light.
        float3 lin = pow(saturate(c), float3(2.4f, 2.4f, 2.4f));
        c = pow(saturate(bt2020To709(lin)), float3(1.0f / 2.4f, 1.0f / 2.4f, 1.0f / 2.4f));
    }
    c = saturate(c);
    float alpha = saturate(rgba.w);
    if (P(2).x > 0.5f) c = c * alpha;
    return float4(c, alpha);
}
