// Cross-backend check: every kernel must produce the same image on the
// Direct3D 11 backend (GPU / WARP) as on the CPU backend.
#if defined(_WIN32)

#include <doctest.h>

#include <cmath>

#include "core/platform.h"
#include "core/strings.h"
#include "render/compositor.h"
#include "render/gpu/gpu.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;
using namespace avc::gpu;

namespace {

std::vector<float> readFloat(Device& d, const Texture& t) {
    std::vector<uint16_t> h(static_cast<size_t>(t.width()) * static_cast<size_t>(t.height()) * 4);
    d.readback(t, h.data(), t.width() * 8);
    std::vector<float> f(h.size());
    for (size_t i = 0; i < h.size(); ++i) f[i] = halfToFloat(h[i]);
    return f;
}

std::unique_ptr<Device> makeD3D() {
    std::string err;
    D3D11DeviceOptions o;
    auto dev = createD3D11Device(o, &err);
    if (!dev) MESSAGE("Direct3D 11 unavailable: ", err);
    return dev;
}

// Deterministic gradient test image.
std::vector<uint16_t> pattern(int w, int h) {
    std::vector<uint16_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            const float a = 0.5f + 0.5f * std::sin(static_cast<float>(x) * 0.3f);
            px[i] = floatToHalf(static_cast<float>(x) / static_cast<float>(w) * a);
            px[i + 1] = floatToHalf(static_cast<float>(y) / static_cast<float>(h) * a);
            px[i + 2] = floatToHalf(0.5f * a);
            px[i + 3] = floatToHalf(a);
        }
    return px;
}

}  // namespace

TEST_CASE("d3d11 kernels match the CPU backend") {
    auto d3d = makeD3D();
    if (!d3d) return;
    auto cpu = createCpuDevice(2);
    MESSAGE("D3D11 adapter: ", d3d->info().adapter);
    const int w = 48, h = 32;
    auto pat = pattern(w, h);
    for (int k = 0; k < static_cast<int>(Kernel::Count); ++k) {
        const Kernel kernel = static_cast<Kernel>(k);
        if (kernel == Kernel::Noise || kernel == Kernel::Glitch) continue;  // hash noise may differ by float rounding
        KernelParams p;
        p.set(0, 0.35f, 0.25f, 1.0f, 3.0f);
        p.set(1, 0.9f, 0.5f, 0.2f, 1.0f);
        p.set(2, 1.0f, 0.0f, 1.0f, 0.0f);
        p.set(3, 1.0f, 1.0f, 40.0f, 30.0f);
        p.set(4, 0.8f, 3.0f, 1.0f, 1.0f);
        std::vector<float> results[2];
        Device* devs[2] = {cpu.get(), d3d.get()};
        for (int d = 0; d < 2; ++d) {
            auto a = devs[d]->createTexture({w, h, TexFormat::RGBA16F, true});
            auto b = devs[d]->createTexture({w, h, TexFormat::RGBA16F, true});
            auto out = devs[d]->createTexture({w, h, TexFormat::RGBA16F, true});
            devs[d]->upload(*a, pat.data(), w * 8);
            devs[d]->upload(*b, pat.data(), w * 8);
            devs[d]->run(kernel, p, {a.get(), b.get(), a.get(), b.get()}, *out);
            results[d] = readFloat(*devs[d], *out);
        }
        double maxErr = 0;
        size_t bad = 0;
        for (size_t i = 0; i < results[0].size(); ++i) {
            const double e = std::fabs(results[0][i] - results[1][i]);
            if (std::isfinite(e)) maxErr = std::max(maxErr, e);
            if (e > 0.02) ++bad;
        }
        // Allow a handful of pixels on hard edges (sampling position rounding).
        CHECK_MESSAGE(bad <= results[0].size() / 50, std::string(kernelSourceName(kernel)), " max error ", maxErr, " bad ", bad);
    }
}

TEST_CASE("d3d11 compositor renders a timeline like the CPU backend") {
    auto d3d = makeD3D();
    if (!d3d) return;
    auto cpu = createCpuDevice(2);
    MediaFrameProvider frames;
    auto text = text::createTextRasterizer();
    Project p = makeProject("X", ProjectSettings{320, 180, {30, 1}, 48000});
    Document doc(std::make_shared<Project>(p));
    Clip bg = makeSolidClip(pv(0.2f, 0.3f, 0.8f, 1), Time{0}, Time::fromSeconds(2));
    Clip box = makeSolidClip(pv(1, 0.5f, 0, 1), Time{0}, Time::fromSeconds(2));
    box.transform.setStatic("scale", pv(40, 40));
    box.transform.setStatic("rotation", pv(30));
    box.blend = BlendMode::Overlay;
    REQUIRE(doc.editSequence("p", doc.project().activeSequence, [&](SequenceEditor& e) {
        return edit::placeClips(e, {{0, bg}, {1, box}}, edit::PlaceMode::Overwrite);
    }));
    Compositor c1(*cpu, frames, text), c2(*d3d, frames, text);
    auto r1 = c1.render(doc.current(), *doc.project().active(), Time::fromSeconds(1), RenderOptions{160, 90});
    auto r2 = c2.render(doc.current(), *doc.project().active(), Time::fromSeconds(1), RenderOptions{160, 90});
    REQUIRE(r1);
    REQUIRE(r2);
    auto a = readbackRgba8(*cpu, **r1);
    auto b = readbackRgba8(*d3d, **r2);
    REQUIRE(a.size() == b.size());
    size_t bad = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > 6) ++bad;
    CHECK(bad < a.size() / 100);
}

#endif
