#include <doctest.h>

#include <cmath>
#include <cstring>

#include "decode/video_frame.h"
#include "render/gpu/gpu.h"

using namespace avc;
using namespace avc::gpu;

namespace {

struct Px {
    float r, g, b, a;
};

// Reads a pixel of an RGBA16F texture (premultiplied values).
Px pixelAt(Device& d, const Texture& t, int x, int y) {
    std::vector<uint16_t> h(static_cast<size_t>(t.width()) * static_cast<size_t>(t.height()) * 4);
    d.readback(t, h.data(), t.width() * 8);
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(t.width()) + static_cast<size_t>(x)) * 4;
    return {halfToFloat(h[i]), halfToFloat(h[i + 1]), halfToFloat(h[i + 2]), halfToFloat(h[i + 3])};
}

TexturePtr solid(Device& d, TexturePool& pool, int w, int h, float r, float g, float b, float a) {
    auto t = pool.acquire(w, h);
    KernelParams p;
    p.set(0, r * a, g * a, b * a, a);
    d.run(Kernel::Fill, p, {}, *t);
    return t;
}

}  // namespace

TEST_CASE("half float conversion") {
    for (float v : {0.0f, 1.0f, -2.5f, 0.333f, 65504.0f, 1e-5f, 0.5f}) CHECK(halfToFloat(floatToHalf(v)) == doctest::Approx(v).epsilon(0.002));
}

TEST_CASE("YUV 4:2:0 limited range converts to expected RGB") {
    auto dev = createCpuDevice(2);
    TexturePool pool(*dev);
    // BT.709 limited: pure red (1,0,0) -> Y=63, Cb=102, Cr=240
    auto f = allocateFrame(16, 16, PixelFormat::YUV420P);
    f->matrix = ColorMatrix::BT709;
    f->fullRange = false;
    std::memset(const_cast<uint8_t*>(f->planes[0]), 63, static_cast<size_t>(f->strides[0]) * 16);
    std::memset(const_cast<uint8_t*>(f->planes[1]), 102, static_cast<size_t>(f->strides[1]) * 8);
    std::memset(const_cast<uint8_t*>(f->planes[2]), 240, static_cast<size_t>(f->strides[2]) * 8);
    auto tex = uploadVideoFrame(*dev, pool, *f);
    REQUIRE(tex);
    Px p = pixelAt(*dev, *tex, 5, 5);
    CHECK(p.r == doctest::Approx(1.0).epsilon(0.02));
    CHECK(p.g == doctest::Approx(0.0).epsilon(0.02));
    CHECK(p.b == doctest::Approx(0.0).epsilon(0.02));
    CHECK(p.a == doctest::Approx(1.0));
    // CPU reference conversion agrees.
    std::vector<uint8_t> rgba(16 * 16 * 4);
    convertToRgba8(*f, rgba.data(), 16 * 4);
    CHECK(std::abs(rgba[0] - p.r * 255) < 3);
}

TEST_CASE("NV12 and 10-bit planar upload") {
    auto dev = createCpuDevice(1);
    TexturePool pool(*dev);
    auto nv = allocateFrame(8, 8, PixelFormat::NV12);
    nv->fullRange = true;
    std::memset(const_cast<uint8_t*>(nv->planes[0]), 128, static_cast<size_t>(nv->strides[0]) * 8);
    std::memset(const_cast<uint8_t*>(nv->planes[1]), 128, static_cast<size_t>(nv->strides[1]) * 4);
    auto t = uploadVideoFrame(*dev, pool, *nv);
    Px p = pixelAt(*dev, *t, 2, 2);
    CHECK(p.r == doctest::Approx(128 / 255.0).epsilon(0.01));
    CHECK(p.g == doctest::Approx(128 / 255.0).epsilon(0.01));

    auto hb = allocateFrame(8, 8, PixelFormat::YUV420P10);
    hb->bitDepth = 10;
    hb->fullRange = true;
    for (int pl = 0; pl < 3; ++pl) {
        auto* d = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(hb->planes[static_cast<size_t>(pl)]));
        const int n = hb->strides[static_cast<size_t>(pl)] / 2 * hb->planeHeight(pl);
        for (int i = 0; i < n; ++i) d[i] = pl == 0 ? 1023 : 512;
    }
    auto t2 = uploadVideoFrame(*dev, pool, *hb);
    Px w = pixelAt(*dev, *t2, 3, 3);
    CHECK(w.r == doctest::Approx(1.0).epsilon(0.01));
    CHECK(w.b == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("composite: transform, opacity and blend modes") {
    auto dev = createCpuDevice(2);
    TexturePool pool(*dev);
    auto bg = solid(*dev, pool, 64, 64, 0.2f, 0.4f, 0.6f, 1.0f);
    auto layer = solid(*dev, pool, 16, 16, 1.0f, 0.0f, 0.0f, 1.0f);
    auto out = pool.acquire(64, 64);
    // Place the 16x16 layer at output (10,20) with identity scale: inverse maps out px -> layer px.
    KernelParams p;
    p.set(0, 1, 0, -10, 0);
    p.set(1, 0, 1, -20, 0);
    p.set(2, 16, 16, 1.0f / 16, 1.0f / 16);
    p.set(3, 0, 0, 16, 16);
    p.set(4, 1.0f, 0, 1.0f, 1.0f);
    dev->run(Kernel::Composite, p, {bg.get(), layer.get()}, *out);
    Px inside = pixelAt(*dev, *out, 15, 25);
    Px outside = pixelAt(*dev, *out, 5, 5);
    CHECK(inside.r == doctest::Approx(1.0).epsilon(0.01));
    CHECK(inside.b == doctest::Approx(0.0).epsilon(0.01));
    CHECK(outside.b == doctest::Approx(0.6).epsilon(0.01));
    // 50% opacity
    p.set(4, 0.5f, 0, 1.0f, 1.0f);
    dev->run(Kernel::Composite, p, {bg.get(), layer.get()}, *out);
    Px half = pixelAt(*dev, *out, 15, 25);
    CHECK(half.r == doctest::Approx(0.6).epsilon(0.01));
    CHECK(half.b == doctest::Approx(0.3).epsilon(0.01));
    // Multiply blend: red * bg = (0.2, 0, 0)
    p.set(4, 1.0f, 1.0f, 1.0f, 1.0f);
    dev->run(Kernel::Composite, p, {bg.get(), layer.get()}, *out);
    Px mul = pixelAt(*dev, *out, 15, 25);
    CHECK(mul.r == doctest::Approx(0.2).epsilon(0.01));
    CHECK(mul.g == doctest::Approx(0.0).epsilon(0.01));
    // Screen blend: 1 - (1-a)(1-b) -> r = 1, g = .4, b = .6
    p.set(4, 1.0f, 2.0f, 1.0f, 1.0f);
    dev->run(Kernel::Composite, p, {bg.get(), layer.get()}, *out);
    Px scr = pixelAt(*dev, *out, 15, 25);
    CHECK(scr.r == doctest::Approx(1.0).epsilon(0.01));
    CHECK(scr.g == doctest::Approx(0.4).epsilon(0.01));
    // Difference: |bg - red| = (0.8, 0.4, 0.6)
    p.set(4, 1.0f, 7.0f, 1.0f, 1.0f);
    dev->run(Kernel::Composite, p, {bg.get(), layer.get()}, *out);
    Px diff = pixelAt(*dev, *out, 15, 25);
    CHECK(diff.r == doctest::Approx(0.8).epsilon(0.01));
}

TEST_CASE("gaussian blur conserves energy and spreads") {
    auto dev = createCpuDevice(2);
    TexturePool pool(*dev);
    auto src = pool.acquire(33, 33);
    dev->clear(*src, 0, 0, 0, 0);
    std::vector<uint16_t> px(33 * 33 * 4, 0);
    const size_t c = (16 * 33 + 16) * 4;
    for (int k = 0; k < 4; ++k) px[c + static_cast<size_t>(k)] = floatToHalf(1.0f);
    dev->upload(*src, px.data(), 33 * 8);
    auto tmp = pool.acquire(33, 33), dst = pool.acquire(33, 33);
    KernelParams h, v;
    h.set(0, 1.0f / 33, 0, 2.0f, 6);
    v.set(0, 0, 1.0f / 33, 2.0f, 6);
    dev->run(Kernel::BlurGaussian, h, {src.get()}, *tmp);
    dev->run(Kernel::BlurGaussian, v, {tmp.get()}, *dst);
    std::vector<uint16_t> out(33 * 33 * 4);
    dev->readback(*dst, out.data(), 33 * 8);
    double total = 0;
    for (size_t i = 3; i < out.size(); i += 4) total += halfToFloat(out[i]);
    CHECK(total == doctest::Approx(1.0).epsilon(0.02));
    CHECK(halfToFloat(out[c + 3]) < 0.1f);
    CHECK(halfToFloat(out[((16 * 33 + 18) * 4) + 3]) > 0.005f);
}

TEST_CASE("rgb to yuv planes roundtrip") {
    auto dev = createCpuDevice(1);
    TexturePool pool(*dev);
    auto src = solid(*dev, pool, 8, 8, 1.0f, 0.5f, 0.25f, 1.0f);
    auto y = dev->createTexture({8, 8, TexFormat::R8, true});
    auto uv = dev->createTexture({4, 4, TexFormat::RG8, true});
    KernelParams p;
    p.set(0, 0, 0);
    p.set(1, 0.2126f, 0.7152f, 0.0722f);
    dev->run(Kernel::RgbToYuv, p, {src.get()}, *y);
    p.set(0, 3, 0);
    dev->run(Kernel::RgbToYuv, p, {src.get()}, *uv);
    std::vector<uint8_t> yb(64), uvb(32);
    dev->readback(*y, yb.data(), 8);
    dev->readback(*uv, uvb.data(), 8);
    auto f = allocateFrame(8, 8, PixelFormat::NV12);
    for (int r = 0; r < 8; ++r) std::memcpy(const_cast<uint8_t*>(f->planes[0]) + r * f->strides[0], yb.data() + r * 8, 8);
    for (int r = 0; r < 4; ++r) std::memcpy(const_cast<uint8_t*>(f->planes[1]) + r * f->strides[1], uvb.data() + r * 8, 8);
    std::vector<uint8_t> rgba(64 * 4);
    convertToRgba8(*f, rgba.data(), 32);
    CHECK(std::abs(rgba[0] - 255) <= 3);
    CHECK(std::abs(rgba[1] - 128) <= 3);
    CHECK(std::abs(rgba[2] - 64) <= 3);
}

TEST_CASE("every kernel runs on the CPU backend") {
    auto dev = createCpuDevice(2);
    TexturePool pool(*dev);
    auto a = solid(*dev, pool, 32, 32, 0.5f, 0.3f, 0.2f, 1.0f);
    auto b = solid(*dev, pool, 32, 32, 0.1f, 0.8f, 0.2f, 1.0f);
    auto out = pool.acquire(32, 32);
    for (int k = 0; k < static_cast<int>(Kernel::Count); ++k) {
        KernelParams p;
        p.set(0, 0.5f, 0.5f, 1.0f, 1.0f);
        p.set(1, 1.0f, 0.5f, 0.5f, 1.0f);
        p.set(2, 1.0f, 0.0f, 1.0f, 0.0f);
        p.set(3, 1.0f, 1.0f, 0.0f, 0.0f);
        dev->run(static_cast<Kernel>(k), p, {a.get(), b.get(), a.get(), b.get()}, *out);
        Px px = pixelAt(*dev, *out, 16, 16);
        CHECK_MESSAGE(std::isfinite(px.r), kernelSourceName(static_cast<Kernel>(k)));
        CHECK_MESSAGE(std::isfinite(px.a), kernelSourceName(static_cast<Kernel>(k)));
    }
}

TEST_CASE("transition kernel endpoints") {
    auto dev = createCpuDevice(1);
    TexturePool pool(*dev);
    auto a = solid(*dev, pool, 16, 16, 1, 0, 0, 1);
    auto b = solid(*dev, pool, 16, 16, 0, 0, 1, 1);
    auto out = pool.acquire(16, 16);
    for (int type : {0, 1, 2, 3, 4, 6, 7, 10, 11, 12}) {
        KernelParams p;
        p.set(0, static_cast<float>(type), 0.0f, 1.0f, 0.05f);
        p.set(1, 0, 0, 0, 1.0f);
        dev->run(Kernel::Transition, p, {a.get(), b.get()}, *out);
        Px s = pixelAt(*dev, *out, 8, 8);
        CHECK_MESSAGE(s.r > 0.9f, "type ", type, " start");
        p.set(0, static_cast<float>(type), 1.0f, 1.0f, 0.05f);
        dev->run(Kernel::Transition, p, {a.get(), b.get()}, *out);
        Px e = pixelAt(*dev, *out, 8, 8);
        CHECK_MESSAGE(e.b > 0.9f, "type ", type, " end");
    }
}
