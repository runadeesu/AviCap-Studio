// Two-input transitions. t0 outgoing A, t1 incoming B (full frame, premultiplied; may be empty).
// P(0): x type, y progress 0..1, z direction (0 left,1 right,2 up,3 down), w softness
// P(1): colour for dips / flash (rgb), w aspect (w/h)
// Types: 0 cross dissolve, 1 dip to colour, 2 slide, 3 push, 4 zoom, 5 blur, 6 spin,
//        7 flash, 8 glitch, 9 whip, 10 wipe, 11 circle wipe, 12 additive dissolve
#include "common.hlsli"

float2 dirVec(float d) {
    if (d < 0.5f) return float2(-1.0f, 0.0f);
    if (d < 1.5f) return float2(1.0f, 0.0f);
    if (d < 2.5f) return float2(0.0f, -1.0f);
    return float2(0.0f, 1.0f);
}

float4 sampleA(float2 uv) { return SAMPLE_BORDER(0, uv); }
float4 sampleB(float2 uv) { return SAMPLE_BORDER(1, uv); }

float4 kernel_main(KERNEL_CTX) {
    int type = (int)P(0).x;
    float p = saturate(P(0).y);
    float2 uv = ctx.uv;
    float2 dir = dirVec(P(0).z);
    float soft = max(P(0).w, 0.001f);
    float4 color = float4(xyz(P(1)), 1.0f);
    if (type == 1) {
        if (p < 0.5f) return lerp(sampleA(uv), color, p * 2.0f);
        return lerp(color, sampleB(uv), (p - 0.5f) * 2.0f);
    }
    if (type == 2) {
        float4 b = sampleB(uv + dir * (1.0f - p));
        float4 a = sampleA(uv);
        return b + a * (1.0f - b.w);
    }
    if (type == 3) {
        float4 a = sampleA(uv - dir * p);
        float4 b = sampleB(uv + dir * (1.0f - p));
        return a + b;
    }
    if (type == 4) {
        float2 c = float2(0.5f, 0.5f);
        float4 a = sampleA(c + (uv - c) / (1.0f + p * 3.0f));
        float4 b = sampleB(c + (uv - c) * (1.0f + (1.0f - p) * 1.5f));
        return lerp(a, b, smoothstep(0.3f, 0.7f, p));
    }
    if (type == 5 || type == 9) {
        float strength = sin(p * kPi);
        float2 v = type == 9 ? dir * (0.35f * strength) : float2(0.02f * strength, 0.02f * strength);
        float2 shift = type == 9 ? dir * (smoothstep(0.0f, 1.0f, p) - (p < 0.5f ? 0.0f : 1.0f)) : float2(0.0f, 0.0f);
        float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
        LOOP for (int i = 0; i < 16; i++) {
            float t = float(i) / 15.0f - 0.5f;
            float2 o = type == 9 ? v * t : float2(v.x * cos(float(i) * 2.4f), v.y * sin(float(i) * 2.4f)) * (float(i) / 15.0f);
            float4 s = p < 0.5f ? sampleA(uv + o - shift) : sampleB(uv + o - shift);
            acc += s;
        }
        return acc / 16.0f;
    }
    if (type == 6) {
        float2 c = float2(0.5f, 0.5f);
        float ang = p * 2.0f * kPi * (p < 0.5f ? 1.0f : -1.0f) * (p < 0.5f ? p : 1.0f - p);
        float2 d = uv - c;
        d.x = d.x * P(1).w;
        float s = sin(ang);
        float co = cos(ang);
        d = float2(d.x * co - d.y * s, d.x * s + d.y * co);
        d.x = d.x / P(1).w;
        float scale = 1.0f + sin(p * kPi) * 0.5f;
        float2 suv = c + d * scale;
        return p < 0.5f ? sampleA(suv) : sampleB(suv);
    }
    if (type == 7) {
        float4 base = lerp(sampleA(uv), sampleB(uv), smoothstep(0.45f, 0.55f, p));
        float f = pow(sin(p * kPi), 4.0f);
        return lerp(base, color, f);
    }
    if (type == 8) {
        float amount = sin(p * kPi);
        float row = floor(uv.y * 24.0f);
        float r = hash12(float2(row, floor(p * 30.0f)));
        float2 guv = uv + float2((r - 0.5f) * 0.15f * amount, 0.0f);
        float4 a = sampleA(guv);
        float4 b = sampleB(guv);
        float4 m = r < p ? b : a;
        float4 shifted = p < 0.5f ? sampleA(guv + float2(0.01f * amount, 0.0f)) : sampleB(guv + float2(0.01f * amount, 0.0f));
        return float4(shifted.x, m.y, m.z, max(m.w, shifted.w));
    }
    if (type == 10) {
        float coord = dot(uv, -dir) + (dir.x + dir.y > 0.0f ? 1.0f : 0.0f);
        float edge = p * (1.0f + soft) - soft;
        float m = smoothstep(edge, edge + soft, coord);
        return lerp(sampleB(uv), sampleA(uv), m);
    }
    if (type == 11) {
        float2 d = uv - float2(0.5f, 0.5f);
        d.x = d.x * P(1).w;
        float r = length(d);
        float maxR = length(float2(0.5f * P(1).w, 0.5f));
        float edge = p * (maxR + soft);
        float m = 1.0f - smoothstep(edge - soft, edge, r);
        return lerp(sampleA(uv), sampleB(uv), m);
    }
    if (type == 12) {
        float4 a = sampleA(uv);
        float4 b = sampleB(uv);
        return saturate(a * min(1.0f, 2.0f * (1.0f - p)) + b * min(1.0f, 2.0f * p));
    }
    return lerp(sampleA(uv), sampleB(uv), p);
}
