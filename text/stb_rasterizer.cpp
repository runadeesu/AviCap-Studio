// Portable text rasterizer based on stb_truetype (used on non-Windows builds
// and as a fallback). Supports UTF-8, kerning, tracking, line spacing, word
// wrapping (including CJK), alignment, per-glyph font fallback, synthetic
// bold/italic and typewriter reveal.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

#include "core/file_io.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "text/text_raster.h"

#include "stb_truetype.h"

namespace avc::text {

namespace {

std::string utf16beToUtf8(const char* data, int len) {
    std::u32string cps;
    for (int i = 0; i + 1 < len; i += 2) {
        const char32_t c = (static_cast<unsigned char>(data[i]) << 8) | static_cast<unsigned char>(data[i + 1]);
        cps.push_back(c);
    }
    return utf32ToUtf8(cps);
}

std::string fontName(const stbtt_fontinfo& info, int nameId) {
    int len = 0;
    // Prefer Microsoft/Unicode English, then any Microsoft/Unicode, then Mac Roman.
    const char* s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                            STBTT_MS_LANG_ENGLISH, nameId);
    if (s && len > 0) return utf16beToUtf8(s, len);
    s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, nameId);
    if (s && len > 0) return std::string(s, static_cast<size_t>(len));
    return {};
}

std::vector<std::filesystem::path> fontDirectories() {
    std::vector<std::filesystem::path> dirs;
#if defined(_WIN32)
    if (auto windir = getEnv("WINDIR")) dirs.push_back(pathFromUtf8(*windir) / "Fonts");
    if (auto local = getEnv("LOCALAPPDATA")) dirs.push_back(pathFromUtf8(*local) / "Microsoft" / "Windows" / "Fonts");
#else
    dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (auto home = getEnv("HOME")) {
        dirs.push_back(pathFromUtf8(*home) / ".fonts");
        dirs.push_back(pathFromUtf8(*home) / ".local" / "share" / "fonts");
    }
#endif
    dirs.push_back(executableDir() / "fonts");
    return dirs;
}

}  // namespace

const std::vector<FontFile>& systemFonts() {
    static const std::vector<FontFile> fonts = [] {
        std::vector<FontFile> out;
        for (const auto& dir : fontDirectories()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code e2;
                if (!it->is_regular_file(e2)) continue;
                const std::string ext = toLower(pathToUtf8(it->path().extension()));
                if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
                if (it->file_size(e2) > (48u << 20)) continue;
                auto data = readFileBytes(it->path());
                if (!data) continue;
                const auto* bytes = reinterpret_cast<const unsigned char*>(data->data());
                const int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
                if (offset < 0) continue;
                stbtt_fontinfo info;
                if (!stbtt_InitFont(&info, bytes, offset)) continue;
                FontFile f;
                f.family = fontName(info, 1);
                if (f.family.empty()) continue;
                const std::string sub = toLower(fontName(info, 2));
                f.bold = sub.find("bold") != std::string::npos || sub.find("black") != std::string::npos ||
                         sub.find("heavy") != std::string::npos;
                f.italic = sub.find("italic") != std::string::npos || sub.find("oblique") != std::string::npos;
                f.cjk = stbtt_FindGlyphIndex(&info, 0x3042) != 0 && stbtt_FindGlyphIndex(&info, 0x6F22) != 0;
                f.path = pathToUtf8(it->path());
                out.push_back(std::move(f));
            }
        }
        std::sort(out.begin(), out.end(), [](const FontFile& a, const FontFile& b) { return a.family < b.family; });
        AVC_INFO("text", "Font catalogue: {} font files", out.size());
        return out;
    }();
    return fonts;
}

namespace {

struct LoadedFont {
    std::string data;
    stbtt_fontinfo info{};
    bool bold = false;
    bool italic = false;
};

class FontCache {
public:
    std::shared_ptr<LoadedFont> load(const FontFile& f) {
        std::lock_guard lock(mutex_);
        auto it = fonts_.find(f.path);
        if (it != fonts_.end()) return it->second;
        auto lf = std::make_shared<LoadedFont>();
        auto data = readFileBytes(pathFromUtf8(f.path));
        if (!data) return nullptr;
        lf->data = std::move(*data);
        const auto* bytes = reinterpret_cast<const unsigned char*>(lf->data.data());
        if (!stbtt_InitFont(&lf->info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0))) return nullptr;
        lf->bold = f.bold;
        lf->italic = f.italic;
        fonts_[f.path] = lf;
        return lf;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<LoadedFont>> fonts_;
};

FontCache& fontCache() {
    static FontCache c;
    return c;
}

// Picks the best file for a family / weight / style; empty family -> default.
const FontFile* findFont(const std::string& family, bool bold, bool italic) {
    const auto& all = systemFonts();
    const FontFile* best = nullptr;
    int bestScore = -1;
    const std::string want = toLower(family);
    for (const auto& f : all) {
        const std::string fam = toLower(f.family);
        int score = 0;
        if (!want.empty() && fam == want) score += 100;
        else if (!want.empty() && (fam.find(want) != std::string::npos || want.find(fam) != std::string::npos)) score += 50;
        else continue;
        if (f.bold == bold) score += 10;
        if (f.italic == italic) score += 5;
        if (score > bestScore) {
            bestScore = score;
            best = &f;
        }
    }
    return best;
}

std::vector<const FontFile*> fallbackChain(bool bold) {
    std::vector<const FontFile*> out;
    // Common Latin UI fonts, then any CJK-capable font.
    for (const char* fam : {"Segoe UI", "Yu Gothic UI", "Meiryo", "Arial", "DejaVu Sans", "Liberation Sans", "FreeSans",
                            "Noto Sans", "IPAGothic", "IPAPGothic", "WenQuanYi Zen Hei", "Noto Sans CJK JP"}) {
        if (const FontFile* f = findFont(fam, bold, false)) out.push_back(f);
    }
    for (const auto& f : systemFonts())
        if (f.cjk && !f.bold) out.push_back(&f);
    if (out.empty() && !systemFonts().empty()) out.push_back(&systemFonts().front());
    return out;
}

struct Glyph {
    char32_t cp;
    LoadedFont* font;
    float x;  // pen position (px) within the line
    float advance;
};

struct Line {
    std::vector<Glyph> glyphs;
    float width = 0;
};

bool isCjk(char32_t c) {
    return (c >= 0x3000 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

class StbRasterizer final : public ITextRasterizer {
public:
    Result<TextRasterPtr> rasterize(const TextRequest& req) override {
        const bool wantBold = req.style.fontWeight >= 600;
        const FontFile* primaryFile = findFont(req.style.fontFamily, wantBold, req.style.italic);
        std::vector<std::shared_ptr<LoadedFont>> fonts;
        if (primaryFile)
            if (auto f = fontCache().load(*primaryFile)) fonts.push_back(f);
        for (const FontFile* ff : fallbackChain(wantBold))
            if (auto f = fontCache().load(*ff)) fonts.push_back(f);
        if (fonts.empty()) return Result<TextRasterPtr>::error("No fonts available");

        const float px = std::max(1.0f, req.style.fontSize * req.scale);
        std::u32string text = utf8ToUtf32(req.text);
        size_t visible = 0;
        for (char32_t c : text)
            if (c != U'\n' && c != U'\r') ++visible;
        if (req.revealChars >= 0 && static_cast<size_t>(req.revealChars) < visible) {
            std::u32string cut;
            size_t n = 0;
            for (char32_t c : text) {
                if (c != U'\n' && c != U'\r') {
                    if (n >= static_cast<size_t>(req.revealChars)) break;
                    ++n;
                }
                cut.push_back(c);
            }
            text = cut;
        }

        auto fontFor = [&](char32_t cp) -> LoadedFont* {
            for (auto& f : fonts)
                if (stbtt_FindGlyphIndex(&f->info, static_cast<int>(cp))) return f.get();
            return fonts.front().get();
        };
        auto emScale = [&](LoadedFont* f) { return stbtt_ScaleForMappingEmToPixels(&f->info, px); };

        LoadedFont* primary = fonts.front().get();
        int ascI, descI, gapI;
        stbtt_GetFontVMetrics(&primary->info, &ascI, &descI, &gapI);
        const float ascent = static_cast<float>(ascI) * emScale(primary);
        const float descent = static_cast<float>(-descI) * emScale(primary);
        const float lineHeight = std::max(px * req.style.lineSpacing, ascent + descent * 0.5f);
        const float tracking = req.style.tracking / 1000.0f * px;
        const float wrap = req.wrapWidth > 0 ? req.wrapWidth * req.scale : 0.0f;

        std::vector<Line> lines(1);
        size_t lastBreak = std::string::npos;  // glyph index in current line after a space
        for (size_t i = 0; i < text.size(); ++i) {
            const char32_t cp = text[i];
            if (cp == U'\r') continue;
            if (cp == U'\n') {
                lines.emplace_back();
                lastBreak = std::string::npos;
                continue;
            }
            Line& line = lines.back();
            LoadedFont* f = fontFor(cp);
            const float s = emScale(f);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f->info, static_cast<int>(cp), &adv, &lsb);
            float advance = static_cast<float>(adv) * s + tracking;
            if (!line.glyphs.empty() && line.glyphs.back().font == f && req.style.kerningEnabled > 0.5f) {
                const int kern = stbtt_GetCodepointKernAdvance(&f->info, static_cast<int>(line.glyphs.back().cp), static_cast<int>(cp));
                line.width += static_cast<float>(kern) * s;
            }
            if (wrap > 0 && line.width + advance > wrap && !line.glyphs.empty() && cp != U' ') {
                Line next;
                if (lastBreak != std::string::npos && lastBreak < line.glyphs.size() && !isCjk(cp)) {
                    next.glyphs.assign(line.glyphs.begin() + static_cast<std::ptrdiff_t>(lastBreak), line.glyphs.end());
                    line.glyphs.resize(lastBreak);
                    // drop trailing spaces
                    while (!line.glyphs.empty() && line.glyphs.back().cp == U' ') line.glyphs.pop_back();
                    float x = 0;
                    for (auto& g : next.glyphs) {
                        g.x = x;
                        x += g.advance;
                    }
                    next.width = x;
                    line.width = line.glyphs.empty() ? 0 : line.glyphs.back().x + line.glyphs.back().advance;
                }
                lines.push_back(std::move(next));
                lastBreak = std::string::npos;
            }
            Line& cur = lines.back();
            cur.glyphs.push_back({cp, f, cur.width, advance});
            cur.width += advance;
            if (cp == U' ' || isCjk(cp)) lastBreak = cur.glyphs.size();
        }
        float boxW = 0;
        for (auto& l : lines) boxW = std::max(boxW, l.width - (l.glyphs.empty() ? 0.0f : tracking));
        if (wrap > 0) boxW = std::max(boxW, 0.0f);
        const float boxH = lineHeight * static_cast<float>(lines.size() - 1) + ascent + descent;
        const float stroke = std::max(0.0f, req.strokeWidth * req.scale);
        const float pad = std::ceil(req.padding * req.scale + stroke + 2.0f + (req.style.italic ? px * 0.25f : 0.0f));

        auto raster = std::make_shared<TextRaster>();
        raster->scale = req.scale;
        raster->width = std::max(1, static_cast<int>(std::ceil(boxW + pad * 2)));
        raster->height = std::max(1, static_cast<int>(std::ceil(boxH + pad * 2)));
        raster->boxX = pad;
        raster->boxY = pad;
        raster->boxW = boxW;
        raster->boxH = boxH;
        raster->lines = static_cast<int>(lines.size());
        raster->characters = visible;
        const int W = raster->width, H = raster->height;
        raster->fill.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);

        std::vector<unsigned char> glyphBuf;
        for (size_t li = 0; li < lines.size(); ++li) {
            const Line& line = lines[li];
            const float lw = line.width - (line.glyphs.empty() ? 0.0f : tracking);
            float offX = pad;
            if (req.style.align == TextAlign::Center) offX += (boxW - lw) * 0.5f;
            else if (req.style.align == TextAlign::Right) offX += boxW - lw;
            const float baseline = pad + ascent + lineHeight * static_cast<float>(li);
            for (const Glyph& g : line.glyphs) {
                if (g.cp == U' ' || g.cp == U'\t') continue;
                const float s = emScale(g.font);
                const float gx = offX + g.x;
                const float fx = gx - std::floor(gx);
                int x0, y0, x1, y1;
                stbtt_GetCodepointBitmapBoxSubpixel(&g.font->info, static_cast<int>(g.cp), s, s, fx, 0, &x0, &y0, &x1, &y1);
                const int gw = x1 - x0, gh = y1 - y0;
                if (gw <= 0 || gh <= 0) continue;
                glyphBuf.assign(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
                stbtt_MakeCodepointBitmapSubpixel(&g.font->info, glyphBuf.data(), gw, gh, gw, s, s, fx, 0, static_cast<int>(g.cp));
                const bool fakeItalic = req.style.italic && !g.font->italic;
                for (int y = 0; y < gh; ++y) {
                    const int ty = static_cast<int>(std::floor(baseline)) + y0 + y;
                    if (ty < 0 || ty >= H) continue;
                    const float shear = fakeItalic ? (static_cast<float>(-(y0 + y))) * 0.2f : 0.0f;
                    for (int x = 0; x < gw; ++x) {
                        const int tx = static_cast<int>(std::floor(gx + shear)) + x0 + x;
                        if (tx < 0 || tx >= W) continue;
                        uint8_t& d = raster->fill[static_cast<size_t>(ty) * static_cast<size_t>(W) + static_cast<size_t>(tx)];
                        d = std::max(d, glyphBuf[static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)]);
                    }
                }
            }
        }
        // Synthetic bold: grow coverage when no bold face was available.
        if (wantBold && !fonts.front()->bold) {
            const int r = std::max(1, static_cast<int>(std::round(px / 28.0f)));
            std::vector<uint8_t> src = raster->fill;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t m = 0;
                    for (int dx = -r; dx <= r; ++dx) {
                        const int sx = std::clamp(x + dx, 0, W - 1);
                        m = std::max(m, src[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(sx)]);
                    }
                    raster->fill[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = m;
                }
        }
        return TextRasterPtr(raster);
    }

    std::vector<std::string> fontFamilies() override {
        std::vector<std::string> out;
        for (auto& f : systemFonts())
            if (out.empty() || out.back() != f.family) out.push_back(f.family);
        return out;
    }

    std::string backendName() const override { return "stb_truetype"; }
};

// LRU cache in front of any rasterizer.
class CachedRasterizer final : public ITextRasterizer {
public:
    explicit CachedRasterizer(std::unique_ptr<ITextRasterizer> inner) : inner_(std::move(inner)) {}

    Result<TextRasterPtr> rasterize(const TextRequest& r) override {
        std::string key = r.text;
        key += '\x1f' + r.style.fontFamily;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "|%g|%d|%d|%d|%g|%g|%g|%g|%g|%d|%g|%g", r.style.fontSize, r.style.fontWeight,
                      r.style.italic ? 1 : 0, static_cast<int>(r.style.align), r.style.tracking, r.style.lineSpacing,
                      r.style.kerningEnabled, r.scale, r.wrapWidth, r.revealChars, r.strokeWidth, r.padding);
        key += buf;
        {
            std::lock_guard lock(mutex_);
            auto it = map_.find(key);
            if (it != map_.end()) {
                lru_.splice(lru_.begin(), lru_, it->second.second);
                return it->second.first;
            }
        }
        auto res = inner_->rasterize(r);
        if (!res) return res;
        std::lock_guard lock(mutex_);
        lru_.push_front(key);
        map_[key] = {*res, lru_.begin()};
        while (lru_.size() > 96) {
            map_.erase(lru_.back());
            lru_.pop_back();
        }
        return res;
    }
    std::vector<std::string> fontFamilies() override { return inner_->fontFamilies(); }
    std::string backendName() const override { return inner_->backendName(); }

private:
    std::unique_ptr<ITextRasterizer> inner_;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::pair<TextRasterPtr, std::list<std::string>::iterator>> map_;
};

}  // namespace

std::unique_ptr<ITextRasterizer> createStbRasterizer() { return std::make_unique<StbRasterizer>(); }

std::shared_ptr<ITextRasterizer> createTextRasterizer() {
#if defined(_WIN32)
    if (auto dw = createDirectWriteRasterizer()) return std::make_shared<CachedRasterizer>(std::move(dw));
#endif
    return std::make_shared<CachedRasterizer>(createStbRasterizer());
}

}  // namespace avc::text
