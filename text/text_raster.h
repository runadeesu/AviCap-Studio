#pragma once
// Text layout + rasterization for titles and subtitles.
//
// The rasterizer produces coverage masks (fill and, when available, a
// geometric outer stroke). Colours, gradients, shadows, glows and background
// boxes are applied by the text_compose kernel so preview and export match.
// Windows uses DirectWrite (system font fallback, CJK shaping, kerning);
// other platforms use stb_truetype with a font catalogue.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "timeline/model.h"

namespace avc::text {

struct TextRequest {
    std::string text;       // UTF-8, '\n' separates lines
    TextStyle style;
    float scale = 1.0f;     // raster pixels per sequence pixel
    float wrapWidth = 0.0f; // sequence px; 0 = no wrapping
    int revealChars = -1;   // typewriter: number of characters shown (-1 = all)
    float strokeWidth = 0;  // sequence px; > 0 requests a stroke mask
    float padding = 0;      // extra transparent border (sequence px) for shadows/glow
};

struct TextRaster {
    int width = 0;   // raster pixels
    int height = 0;
    float scale = 1.0f;  // raster px per sequence px
    std::vector<uint8_t> fill;    // coverage, width*height
    std::vector<uint8_t> stroke;  // outer stroke coverage (empty -> derive by dilation)
    // Tight layout box inside the raster (raster px) - used for background boxes.
    float boxX = 0, boxY = 0, boxW = 0, boxH = 0;
    int lines = 0;
    size_t characters = 0;
    [[nodiscard]] float sequenceWidth() const { return static_cast<float>(width) / scale; }
    [[nodiscard]] float sequenceHeight() const { return static_cast<float>(height) / scale; }
};

using TextRasterPtr = std::shared_ptr<const TextRaster>;

class ITextRasterizer {
public:
    virtual ~ITextRasterizer() = default;
    virtual Result<TextRasterPtr> rasterize(const TextRequest& req) = 0;
    [[nodiscard]] virtual std::vector<std::string> fontFamilies() = 0;
    [[nodiscard]] virtual std::string backendName() const = 0;
};

// Platform default (DirectWrite on Windows, stb_truetype elsewhere), wrapped in
// an LRU cache keyed by the request.
std::shared_ptr<ITextRasterizer> createTextRasterizer();
std::unique_ptr<ITextRasterizer> createStbRasterizer();
#if defined(_WIN32)
std::unique_ptr<ITextRasterizer> createDirectWriteRasterizer();
#endif

// Font files discovered on the system (family name -> file). Used by the stb
// backend and the font picker.
struct FontFile {
    std::string family;
    std::string path;
    bool bold = false;
    bool italic = false;
    bool cjk = false;  // covers Japanese kana/kanji
};
const std::vector<FontFile>& systemFonts();

}  // namespace avc::text
