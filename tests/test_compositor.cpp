#include <doctest.h>

#include <cmath>

#include "effects/effects.h"
#include "media/probe.h"
#include "render/compositor.h"
#include "tests/test_support.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

using namespace avc;
using namespace avc::edit;

namespace {

Time S(double s) { return Time::fromSeconds(s); }

struct RenderFixture {
    std::unique_ptr<gpu::Device> dev = gpu::createCpuDevice(4);
    MediaFrameProvider frames;
    std::shared_ptr<text::ITextRasterizer> text = text::createTextRasterizer();
    Compositor comp{*dev, frames, text};
    Document doc{std::make_shared<Project>(makeProject("R", ProjectSettings{640, 360, {30, 1}, 48000}))};

    SequenceId seq() const { return doc.project().activeSequence; }
    const Sequence& s() const { return *doc.project().active(); }

    std::vector<uint8_t> render(Time t, int w = 320, int h = 180) {
        auto r = comp.render(doc.current(), s(), t, RenderOptions{w, h});
        REQUIRE_MESSAGE(r, r.errorMessage());
        return gpu::readbackRgba8(*dev, **r);
    }
    MediaId import(const std::string& path) {
        auto m = createMediaItem(path);
        REQUIRE_MESSAGE(m, m.errorMessage());
        const MediaId id = m->id;
        REQUIRE(doc.edit("import", [&](ProjectEditor& pe) {
            pe.addMedia(*m);
            return Status::ok();
        }));
        return id;
    }
    Status apply(const std::function<Status(SequenceEditor&)>& fn) { return doc.editSequence("op", seq(), fn); }
};

std::array<int, 4> px(const std::vector<uint8_t>& img, int w, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return {img[i], img[i + 1], img[i + 2], img[i + 3]};
}

double meanChannel(const std::vector<uint8_t>& img, int c) {
    double s = 0;
    for (size_t i = static_cast<size_t>(c); i < img.size(); i += 4) s += img[i];
    return s / static_cast<double>(img.size() / 4);
}

}  // namespace

TEST_CASE("empty timeline renders opaque background") {
    RenderFixture f;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        e.props().backgroundColor = pv(0.0f, 0.0f, 1.0f, 1.0f);
        return Status::ok();
    }));
    auto img = f.render(S(0));
    auto p = px(img, 320, 10, 10);
    CHECK(p[2] == 255);
    CHECK(p[3] == 255);
}

TEST_CASE("solid, opacity, transform and blend") {
    RenderFixture f;
    Clip red = makeSolidClip(pv(1, 0, 0, 1), S(0), S(2));
    Clip blue = makeSolidClip(pv(0, 0, 1, 1), S(0), S(2));
    blue.transform.setStatic("scale", pv(50, 50));
    blue.transform.setStatic("position", pv(160, 0));  // move right by 1/4 of 640
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, red}, {1, blue}}, PlaceMode::Overwrite); }));
    auto img = f.render(S(1));
    auto left = px(img, 320, 40, 90);
    auto right = px(img, 320, 240, 90);  // blue centred at (480,180) seq -> (240,90)
    CHECK(left[0] > 250);
    CHECK(right[2] > 250);
    CHECK(right[0] < 5);
    // 50% opacity on the blue layer blends.
    const ClipId blueId = f.s().tracks[1]->clips[0]->id;
    REQUIRE(f.apply([&](SequenceEditor& e) {
        e.mutableClip(blueId).transform.setStatic("opacity", pv(50));
        return Status::ok();
    }));
    img = f.render(S(1));
    right = px(img, 320, 240, 90);
    CHECK(std::abs(right[0] - 128) <= 3);
    CHECK(std::abs(right[2] - 128) <= 3);
    // Rotation 90 degrees keeps the square covering the centre point.
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto& c = e.mutableClip(blueId);
        c.transform.setStatic("opacity", pv(100));
        c.transform.setStatic("rotation", pv(90));
        c.blend = BlendMode::Screen;
        return Status::ok();
    }));
    img = f.render(S(1));
    right = px(img, 320, 240, 90);
    CHECK(right[0] > 250);  // screen(red, blue) = magenta
    CHECK(right[2] > 250);
}

TEST_CASE("keyframed opacity animates over time") {
    RenderFixture f;
    Clip white = makeSolidClip(pv(1, 1, 1, 1), S(0), S(2));
    white.transform.getOrAdd("opacity").addKey({S(0), pv(0)});
    white.transform.getOrAdd("opacity").addKey({S(2), pv(100)});
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, white}}, PlaceMode::Overwrite); }));
    auto a = f.render(S(0.5));
    auto b = f.render(S(1.5));
    CHECK(std::abs(px(a, 320, 5, 5)[0] - 64) <= 3);
    CHECK(std::abs(px(b, 320, 5, 5)[0] - 191) <= 3);
}

TEST_CASE("media clip renders decoded frames with fit") {
    RenderFixture f;
    const std::string path = test::testMedia("scenes.mp4");
    if (path.empty()) return;
    auto m = f.import(path);
    REQUIRE(f.apply([&](SequenceEditor& e) {
        return addMediaClip(e, *f.doc.project().findMedia(m), S(0), 0, -1, PlaceMode::Overwrite).status();
    }));
    auto img = f.render(S(1));
    CHECK(meanChannel(img, 0) > 170);  // red scene fills the 16:9 frame
    img = f.render(S(3));
    CHECK(meanChannel(img, 1) > 90);   // green
    img = f.render(S(5));
    CHECK(meanChannel(img, 2) > 170);  // blue
}

TEST_CASE("cross dissolve between two clips") {
    RenderFixture f;
    Clip a = makeSolidClip(pv(1, 0, 0, 1), S(0), S(2));
    Clip b = makeSolidClip(pv(0, 0, 1, 1), S(2), S(2));
    b.transitionIn = TransitionSpec{"cross-dissolve", S(1), {}};
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, a}, {0, b}}, PlaceMode::Overwrite); }));
    auto mid = f.render(S(2));
    auto p = px(mid, 320, 100, 100);
    CHECK(std::abs(p[0] - 128) <= 4);
    CHECK(std::abs(p[2] - 128) <= 4);
    auto early = f.render(S(1.4));
    CHECK(px(early, 320, 100, 100)[0] > 250);
    auto late = f.render(S(2.6));
    CHECK(px(late, 320, 100, 100)[2] > 250);
}

TEST_CASE("fade in from black and dip to white") {
    RenderFixture f;
    Clip a = makeSolidClip(pv(1, 1, 0, 1), S(0), S(2));
    a.transitionIn = TransitionSpec{"cross-dissolve", S(1), {}};
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, a}}, PlaceMode::Overwrite); }));
    auto img = f.render(S(0.5));
    auto p = px(img, 320, 50, 50);
    CHECK(std::abs(p[0] - 128) <= 4);  // half-way from black background
    CHECK(p[2] < 4);
}

TEST_CASE("title renders text with stroke and background") {
    RenderFixture f;
    Clip t = makeTextClip("AVICAP", S(0), S(3));
    t.textStyle.fontFamily = "DejaVu Sans";
    t.textStyle.fontSize = 100;
    t.textParams.setStatic("fillColor", pv(1, 1, 0, 1));
    t.textParams.setStatic("strokeWidth", pv(4));
    t.textParams.setStatic("backgroundColor", pv(0, 0, 1, 0.5f));
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{1, t}}, PlaceMode::Overwrite); }));
    auto img = f.render(S(1), 640, 360);
    int yellow = 0, blueish = 0;
    for (size_t i = 0; i < img.size(); i += 4) {
        if (img[i] > 200 && img[i + 1] > 200 && img[i + 2] < 60) ++yellow;
        if (img[i + 2] > 100 && img[i] < 30) ++blueish;
    }
    if (text::systemFonts().empty()) return;
    CHECK(yellow > 2000);
    CHECK(blueish > 2000);
    // Outside the text box stays background black.
    auto corner = px(img, 640, 5, 5);
    CHECK(corner[0] < 5);
    // Subtitle sits in the lower part of the frame.
    Clip sub = makeTextClip("字幕テスト subtitle", S(0), S(3), ClipKind::Subtitle);
    sub.textStyle.fontFamily = "DejaVu Sans";
    REQUIRE(f.apply([&](SequenceEditor& e) {
        auto tr = addTrack(e, TrackKind::Subtitle);
        if (!tr) return tr.status();
        return placeClips(e, {{e.seq().trackIndex(*tr), sub}}, PlaceMode::Overwrite);
    }));
    img = f.render(S(1), 640, 360);
    int whiteLow = 0, whiteHigh = 0;
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
            auto p = px(img, 640, x, y);
            if (p[0] > 230 && p[1] > 230 && p[2] > 230) (y > 270 ? whiteLow : whiteHigh)++;
        }
    CHECK(whiteLow > 300);
}

TEST_CASE("adjustment layer applies effects to layers below") {
    RenderFixture f;
    Clip gray = makeSolidClip(pv(0.5f, 0.5f, 0.5f, 1), S(0), S(2));
    Clip adj = makeAdjustmentClip(S(0), S(2));
    EffectInstance cc = fx::EffectRegistry::instance().instantiate("color.basic");
    cc.params.setStatic("exposure", pv(1.0f));
    adj.effects.push_back(cc);
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, gray}, {1, adj}}, PlaceMode::Overwrite); }));
    auto img = f.render(S(1));
    CHECK(px(img, 320, 10, 10)[0] > 150);
}

TEST_CASE("compound clip renders nested sequence") {
    RenderFixture f;
    Clip green = makeSolidClip(pv(0, 1, 0, 1), S(1), S(2));
    REQUIRE(f.apply([&](SequenceEditor& e) { return placeClips(e, {{0, green}}, PlaceMode::Overwrite); }));
    const ClipId gid = f.s().tracks[0]->clips[0]->id;
    REQUIRE(f.doc.edit("nest", [&](ProjectEditor& pe) { return createCompoundClip(pe, f.seq(), {gid}, "N").status(); }));
    CHECK(f.s().tracks[0]->clips[0]->kind == ClipKind::Compound);
    auto img = f.render(S(2));
    CHECK(px(img, 320, 160, 90)[1] > 250);
}

TEST_CASE("video effects run through the registry") {
    RenderFixture f;
    for (const auto* def : fx::EffectRegistry::instance().list(fx::EffectKind::Video)) {
        Clip c = makeSolidClip(pv(0.6f, 0.4f, 0.2f, 1), S(0), S(1));
        c.effects.push_back(fx::EffectRegistry::instance().instantiate(def->id));
        Document d(std::make_shared<Project>(makeProject("E", ProjectSettings{160, 90, {30, 1}, 48000})));
        REQUIRE(d.editSequence("p", d.project().activeSequence, [&](SequenceEditor& e) {
            return placeClips(e, {{0, c}}, PlaceMode::Overwrite);
        }));
        auto r = f.comp.render(d.current(), *d.project().active(), S(0.5), RenderOptions{160, 90});
        REQUIRE_MESSAGE(r, def->id);
        auto img = gpu::readbackRgba8(*f.dev, **r);
        CHECK_MESSAGE(img.size() == 160 * 90 * 4, def->id);
    }
    CHECK(fx::EffectRegistry::instance().list(fx::EffectKind::Video).size() >= 25);
}

TEST_CASE("layer geometry math") {
    Clip c = makeSolidClip(pv(1, 1, 1, 1), S(0), S(1));
    c.kind = ClipKind::Media;
    // 1280x720 source into 1920x1080 fit -> scale 1.5, centred.
    auto g = computeLayerGeometry(c, Time{0}, 1280, 720, 0, {1, 1}, 1920, 1080);
    float x = 0, y = 0;
    g.toSequence.apply(x, y);
    CHECK(x == doctest::Approx(0));
    CHECK(y == doctest::Approx(0));
    x = 1280;
    y = 720;
    g.toSequence.apply(x, y);
    CHECK(x == doctest::Approx(1920));
    CHECK(y == doctest::Approx(1080));
    // Portrait phone video (rotation metadata 90) pillarboxed.
    g = computeLayerGeometry(c, Time{0}, 1920, 1080, 90, {1, 1}, 1920, 1080);
    x = 960;
    y = 540;
    g.toSequence.apply(x, y);
    CHECK(x == doctest::Approx(960));
    CHECK(y == doctest::Approx(540));
    Affine inv = g.toSequence.inverse();
    float cx = 960, cy = 0;
    inv.apply(cx, cy);
    CHECK((cx < 1 || cx > 1919 || cy < 1 || cy > 1079));  // frame top maps to a texture edge
    // Crop 10% left
    c.transform.setStatic("cropLeft", pv(10));
    g = computeLayerGeometry(c, Time{0}, 1000, 500, 0, {1, 1}, 1000, 500);
    CHECK(g.cropX0 == doctest::Approx(100));
}
