#include <doctest.h>

#include <cmath>
#include <thread>

#include "core/platform.h"
#include "core/process.h"
#include "core/strings.h"
#include "decode/video_decoder.h"
#include "media/probe.h"
#include "proxy/proxy.h"
#include "render/compositor.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;

TEST_CASE("proxy keeps timestamps and renders identically") {
    const std::string src = test::testMedia("ntsc_720p.mp4");
    if (src.empty()) return;
    auto dir = test::makeTempDir("proxy");
    const std::string out = pathToUtf8(dir / "p.mov");
    float lastProgress = 0;
    Status st = proxy::generateProxy({src, out, 240}, CancelToken::create(), [&](float f) { lastProgress = f; });
    REQUIRE_MESSAGE(st, st.message());
    CHECK(lastProgress == doctest::Approx(1.0f));
    auto info = probeMedia(out);
    REQUIRE(info);
    CHECK(info->video[0].height == 240);
    CHECK(info->video[0].codec == "mjpeg");
    // Frame-accurate: every proxy frame has the original pts.
    auto a = openVideoDecoder(src);
    auto b = openVideoDecoder(out);
    REQUIRE(a);
    REQUIRE(b);
    for (int i : {0, 7, 30, 61, 100}) {
        const Time t = Time::fromFrames(i, {30000, 1001}) + Time{1000};
        auto fa = (*a)->frameAt(t);
        auto fb = (*b)->frameAt(t);
        REQUIRE(fa);
        REQUIRE(fb);
        CHECK((*fa)->pts == (*fb)->pts);
    }
    // Compositing with the proxy produces the same layout.
    auto m = createMediaItem(src);
    REQUIRE(m);
    Document doc(std::make_shared<Project>(makeProject("P", ProjectSettings{640, 360, {30, 1}, 48000})));
    REQUIRE(doc.edit("i", [&](ProjectEditor& pe) {
        pe.addMedia(*m);
        return Status::ok();
    }));
    REQUIRE(doc.editSequence("a", doc.project().activeSequence, [&](SequenceEditor& e) {
        auto r = edit::addMediaClip(e, *doc.project().findMedia(m->id), Time{0}, 0, -1, edit::PlaceMode::Overwrite);
        if (!r) return r.status();
        e.mutableClip((*r)[0]).transform.setStatic("scale", pv(50, 50));
        e.mutableClip((*r)[0]).transform.setStatic("anchor", pv(200, 0));
        return Status::ok();
    }));
    auto dev = gpu::createCpuDevice(2);
    MediaFrameProvider original, proxied;
    proxied.setProxyResolver([&](const MediaItem&) { return out; });
    proxied.setUseProxies(true);
    Compositor c1(*dev, original, nullptr), c2(*dev, proxied, nullptr);
    auto r1 = c1.render(doc.current(), *doc.project().active(), Time::fromSeconds(1), RenderOptions{160, 90});
    auto r2 = c2.render(doc.current(), *doc.project().active(), Time::fromSeconds(1), RenderOptions{160, 90});
    REQUIRE(r1);
    REQUIRE(r2);
    auto i1 = gpu::readbackRgba8(*dev, **r1), i2 = gpu::readbackRgba8(*dev, **r2);
    double diff = 0;
    for (size_t i = 0; i < i1.size(); ++i) diff += std::abs(int(i1[i]) - int(i2[i]));
    CHECK(diff / static_cast<double>(i1.size()) < 12.0);  // same layout, proxy is only softer
}

TEST_CASE("proxy manager caches by identity") {
    const std::string src = test::testMedia("vp9.webm");
    if (src.empty()) return;
    auto dir = test::makeTempDir("proxymgr");
    proxy::ProxyManager mgr(dir);
    mgr.setWorkerExecutable("");  // in-process for the test
    auto m = createMediaItem(src);
    REQUIRE(m);
    CHECK(mgr.proxyPath(*m, 360).empty());
    mgr.request(*m, 360);
    for (int i = 0; i < 400; ++i) {
        const auto s = mgr.status(m->id).state;
        if (s == proxy::ProxyState::Ready || s == proxy::ProxyState::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(mgr.status(m->id).state == proxy::ProxyState::Ready);
    CHECK_FALSE(mgr.proxyPath(*m, 360).empty());
    MediaItem changed = *m;
    changed.fileModifiedNs += 1;  // source changed -> proxy is stale
    CHECK(mgr.proxyPath(changed, 360).empty());
}

TEST_CASE("child process output and cancellation") {
#if !defined(_WIN32)
    std::vector<std::string> lines;
    auto r = runProcess("/bin/sh", {"-c", "echo one; echo two; exit 3"}, [&](const std::string& l) { lines.push_back(l); });
    REQUIRE(r);
    CHECK(r->exitCode == 3);
    CHECK(lines == std::vector<std::string>{"one", "two"});
    auto token = CancelToken::create();
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        token.cancel();
    });
    auto r2 = runProcess("/bin/sh", {"-c", "sleep 10"}, {}, token);
    killer.join();
    REQUIRE(r2);
    CHECK(r2->cancelled);
#endif
}
