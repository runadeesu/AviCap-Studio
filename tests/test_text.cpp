#include <doctest.h>

#include <algorithm>

#include "text/text_raster.h"

using namespace avc;
using namespace avc::text;

namespace {
size_t coverage(const TextRaster& r) {
    size_t n = 0;
    for (uint8_t v : r.fill) n += v > 128 ? 1 : 0;
    return n;
}
}  // namespace

TEST_CASE("text rasterizes latin and japanese with fallback") {
    auto rz = createTextRasterizer();
    if (systemFonts().empty()) {
        MESSAGE("no system fonts; skipping");
        return;
    }
    TextRequest req;
    req.text = "Hello AviCap";
    req.style.fontFamily = "DejaVu Sans";
    req.style.fontSize = 48;
    auto r = rz->rasterize(req);
    REQUIRE_MESSAGE(r, r.errorMessage());
    CHECK((*r)->width > 200);
    CHECK((*r)->height > 40);
    CHECK(coverage(**r) > 500);
    CHECK((*r)->lines == 1);

    TextRequest jp = req;
    jp.text = "こんにちは世界";
    auto rj = rz->rasterize(jp);
    REQUIRE(rj);
    const bool hasCjk = std::any_of(systemFonts().begin(), systemFonts().end(), [](const FontFile& f) { return f.cjk; });
    if (hasCjk) CHECK(coverage(**rj) > 500);

    // Multi-line + wrapping.
    TextRequest ml = req;
    ml.text = "one two three four five six seven";
    ml.wrapWidth = 300;
    auto rm = rz->rasterize(ml);
    REQUIRE(rm);
    CHECK((*rm)->lines >= 2);
    CHECK((*rm)->boxW <= 300 * ml.scale + 1);

    // Reveal (typewriter) shows fewer glyphs.
    TextRequest rv = req;
    rv.revealChars = 3;
    auto rr = rz->rasterize(rv);
    REQUIRE(rr);
    CHECK(coverage(**rr) < coverage(**r));
    CHECK((*rr)->characters == 12);

    // Scale doubles raster size.
    TextRequest big = req;
    big.scale = 2.0f;
    auto rb = rz->rasterize(big);
    REQUIRE(rb);
    CHECK((*rb)->width == doctest::Approx((*r)->width * 2).epsilon(0.08));
    // Cache returns the same object for identical requests.
    auto again = rz->rasterize(req);
    CHECK(again->get() == r->get());
}
