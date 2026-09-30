// Shared helpers for AviCap kernels (HLSL subset, also compiled as C++).
// Colours are premultiplied RGBA in gamma-encoded Rec.709 working space.

STATIC_CONST float kPi = 3.14159265f;

float luma709(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

float4 unpremultiply(float4 c) {
    if (c.w <= 0.00001f) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return float4(xyz(c) / c.w, c.w);
}

float4 premultiply(float4 c) { return float4(xyz(c) * c.w, c.w); }

float3 rgb2hsv(float3 c) {
    float cmax = max(c.x, max(c.y, c.z));
    float cmin = min(c.x, min(c.y, c.z));
    float d = cmax - cmin;
    float h = 0.0f;
    if (d > 0.00001f) {
        if (cmax == c.x) h = (c.y - c.z) / d;
        else if (cmax == c.y) h = 2.0f + (c.z - c.x) / d;
        else h = 4.0f + (c.x - c.y) / d;
        h = h / 6.0f;
        if (h < 0.0f) h = h + 1.0f;
    }
    float s = cmax > 0.00001f ? d / cmax : 0.0f;
    return float3(h, s, cmax);
}

float3 hsv2rgb(float3 c) {
    float h = frac(c.x) * 6.0f;
    float s = c.y;
    float v = c.z;
    float f = frac(h);
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    float i = floor(h);
    if (i < 1.0f) return float3(v, t, p);
    if (i < 2.0f) return float3(q, v, p);
    if (i < 3.0f) return float3(p, v, t);
    if (i < 4.0f) return float3(p, q, v);
    if (i < 5.0f) return float3(t, p, v);
    return float3(v, p, q);
}

// Hash-based pseudo random in [0,1).
float hash12(float2 p) {
    float3 p3 = frac(float3(p.x, p.y, p.x) * 0.1031f);
    p3 = p3 + dot(p3, float3(p3.y + 33.33f, p3.z + 33.33f, p3.x + 33.33f));
    return frac((p3.x + p3.y) * p3.z);
}

float valueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float a = hash12(i);
    float b = hash12(i + float2(1.0f, 0.0f));
    float c = hash12(i + float2(0.0f, 1.0f));
    float d = hash12(i + float2(1.0f, 1.0f));
    float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// atan without relying on the intrinsic (not supported by every HLSL compiler).
float atanPoly(float x) {
    float ax = abs(x);
    float inv = ax > 1.0f ? 1.0f / ax : ax;
    float z = inv * inv;
    float r = inv * (0.99997726f + z * (-0.33262347f + z * (0.19354346f + z * (-0.11643287f + z * (0.05265332f - 0.01172120f * z)))));
    if (ax > 1.0f) r = 1.5707963f - r;
    return x < 0.0f ? -r : r;
}

bool insideUnit(float2 uv) { return uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f; }

// Sample with transparent border.
#define SAMPLE_BORDER(i, uvv) (insideUnit(uvv) ? SAMPLE(i, uvv) : float4(0.0f, 0.0f, 0.0f, 0.0f))

// ---- separable blend modes (W3C compositing), non-premultiplied inputs --------
float blendChannel(float b, float s, int mode) {
    if (mode == 1) return b * s;                                   // multiply
    if (mode == 2) return b + s - b * s;                           // screen
    if (mode == 3) return b <= 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);  // overlay
    if (mode == 4) {                                               // soft light
        if (s <= 0.5f) return b - (1.0f - 2.0f * s) * b * (1.0f - b);
        float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : sqrt(b);
        return b + (2.0f * s - 1.0f) * (d - b);
    }
    if (mode == 5) return s <= 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);  // hard light
    if (mode == 6) return min(1.0f, b + s);                        // add (linear dodge)
    if (mode == 7) return abs(b - s);                              // difference
    if (mode == 8) return min(b, s);                               // darken
    if (mode == 9) return max(b, s);                               // lighten
    return s;                                                      // normal
}

// Composites premultiplied `src` over premultiplied `dst` with a blend mode.
float4 blendOver(float4 dst, float4 src, int mode) {
    if (mode == 0) return src + dst * (1.0f - src.w);
    if (mode == 6) {
        // Add: simple additive on premultiplied values.
        return float4(min(xyz(dst) + xyz(src), float3(1.0f, 1.0f, 1.0f)), src.w + dst.w * (1.0f - src.w));
    }
    float4 cs = unpremultiply(src);
    float4 cb = unpremultiply(dst);
    float3 bl = float3(blendChannel(cb.x, cs.x, mode), blendChannel(cb.y, cs.y, mode), blendChannel(cb.z, cs.z, mode));
    float3 co = xyz(src) * (1.0f - dst.w) + xyz(dst) * (1.0f - src.w) + src.w * dst.w * bl;
    return float4(co, src.w + dst.w * (1.0f - src.w));
}
