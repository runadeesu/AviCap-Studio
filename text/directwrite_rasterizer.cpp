// DirectWrite + Direct2D text rasterizer (Windows). Provides proper shaping,
// system font fallback (Japanese/Chinese/Korean/emoji), kerning, tracking and
// geometric outer strokes from glyph outlines.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "core/log.h"
#include "core/strings.h"
#include "text/text_raster.h"

namespace avc::text {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Collects glyph outlines of a layout into one path geometry (for strokes).
class OutlineCollector final : public IDWriteTextRenderer {
public:
    OutlineCollector(ID2D1Factory* factory, ID2D1GeometrySink* sink, float offsetX, float offsetY)
        : factory_(factory), sink_(sink), ox_(offsetX), oy_(offsetY) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IDWriteTextRenderer) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IUnknown)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override {
        *ppd = 1.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baselineX, FLOAT baselineY, DWRITE_MEASURING_MODE,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override {
        Com<ID2D1PathGeometry> geo;
        if (FAILED(factory_->CreatePathGeometry(geo.put()))) return S_OK;
        Com<ID2D1GeometrySink> gs;
        if (FAILED(geo->Open(gs.put()))) return S_OK;
        run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                          run->glyphCount, run->isSideways, run->bidiLevel % 2, gs.p);
        gs->Close();
        const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Translation(baselineX + ox_, baselineY + oy_);
        Com<ID2D1TransformedGeometry> tg;
        if (SUCCEEDED(factory_->CreateTransformedGeometry(geo.p, m, tg.put()))) {
            tg->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, nullptr, sink_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }

private:
    ID2D1Factory* factory_;
    ID2D1GeometrySink* sink_;
    float ox_, oy_;
    ULONG refs_ = 1;
};

class DirectWriteRasterizer final : public ITextRasterizer {
public:
    bool init() {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dw_.put()))))
            return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), nullptr,
                                     reinterpret_cast<void**>(d2d_.put()))))
            return false;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                    reinterpret_cast<void**>(wic_.put()))))
            return false;
        return true;
    }

    Result<TextRasterPtr> rasterize(const TextRequest& req) override {
        std::lock_guard lock(mutex_);
        std::u32string cps = utf8ToUtf32(req.text);
        size_t visible = 0;
        for (char32_t c : cps)
            if (c != U'\n' && c != U'\r') ++visible;
        if (req.revealChars >= 0 && static_cast<size_t>(req.revealChars) < visible) {
            std::u32string cut;
            size_t n = 0;
            for (char32_t c : cps) {
                if (c != U'\n' && c != U'\r') {
                    if (n >= static_cast<size_t>(req.revealChars)) break;
                    ++n;
                }
                cut.push_back(c);
            }
            cps = cut;
        }
        std::wstring text = utf8ToWide(utf32ToUtf8(cps));
        if (text.empty()) text = L" ";
        const float px = std::max(1.0f, req.style.fontSize * req.scale);
        bool cjk = false;
        for (char32_t c : cps)
            if ((c >= 0x3000 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xAC00 && c <= 0xD7AF))
                cjk = true;
        const std::wstring family = resolveFamily(utf8ToWide(req.style.fontFamily.empty() ? "Yu Gothic UI" : req.style.fontFamily), cps, cjk);
        // No installed family can draw this text through DirectWrite: let the
        // fallback rasterizer (which scans font files) handle it.
        if (family.empty()) return Result<TextRasterPtr>::error("no installed font covers this text");
        Com<IDWriteTextFormat> fmt;
        HRESULT hr = dw_->CreateTextFormat(family.c_str(), nullptr,
                                           static_cast<DWRITE_FONT_WEIGHT>(std::clamp(req.style.fontWeight, 100, 950)),
                                           req.style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, px, L"ja-jp", fmt.put());
        if (FAILED(hr)) return Result<TextRasterPtr>::error("CreateTextFormat failed");
        fmt->SetTextAlignment(req.style.align == TextAlign::Left    ? DWRITE_TEXT_ALIGNMENT_LEADING
                              : req.style.align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                    : DWRITE_TEXT_ALIGNMENT_CENTER);
        const bool wrap = req.wrapWidth > 0;
        fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        const float lineH = px * req.style.lineSpacing;
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, lineH * 0.8f);
        Com<IDWriteTextLayout> layout;
        const float maxW = wrap ? req.wrapWidth * req.scale : 100000.0f;
        hr = dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.p, maxW, 100000.0f, layout.put());
        if (FAILED(hr)) return Result<TextRasterPtr>::error("CreateTextLayout failed");
        if (req.style.tracking != 0.0f) {
            Com<IDWriteTextLayout1> l1;
            if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(l1.put()))))
                l1->SetCharacterSpacing(0.0f, req.style.tracking / 1000.0f * px, 0.0f,
                                        DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        if (!wrap) {
            layout->SetMaxWidth(std::max(1.0f, m.widthIncludingTrailingWhitespace));
            layout->GetMetrics(&m);
        }
        DWRITE_OVERHANG_METRICS oh{};
        layout->GetOverhangMetrics(&oh);
        const float stroke = std::max(0.0f, req.strokeWidth * req.scale);
        const float extra = std::max({0.0f, oh.left, oh.right, oh.top, oh.bottom});
        const float pad = std::ceil(req.padding * req.scale + stroke + 2.0f + extra);
        const float boxW = wrap ? layout->GetMaxWidth() : m.width;
        const float boxH = m.height;

        auto raster = std::make_shared<TextRaster>();
        raster->scale = req.scale;
        raster->width = std::max(1, static_cast<int>(std::ceil(boxW + 2 * pad)));
        raster->height = std::max(1, static_cast<int>(std::ceil(boxH + 2 * pad)));
        raster->boxX = pad + (wrap ? 0.0f : 0.0f);
        raster->boxY = pad;
        raster->boxW = boxW;
        raster->boxH = boxH;
        raster->lines = static_cast<int>(m.lineCount);
        raster->characters = visible;
        const float originX = pad - (wrap ? 0.0f : m.left);
        const float originY = pad;

        auto render = [&](bool strokePass, std::vector<uint8_t>& mask) -> bool {
            Com<IWICBitmap> bmp;
            if (FAILED(wic_->CreateBitmap(static_cast<UINT>(raster->width), static_cast<UINT>(raster->height),
                                          GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put())))
                return false;
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Com<ID2D1RenderTarget> rt;
            if (FAILED(d2d_->CreateWicBitmapRenderTarget(bmp.p, props, rt.put()))) return false;
            Com<ID2D1SolidColorBrush> brush;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.put());
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            if (!strokePass) {
                rt->DrawTextLayout(D2D1::Point2F(originX, originY), layout.p, brush.p, D2D1_DRAW_TEXT_OPTIONS_NONE);
            } else {
                Com<ID2D1PathGeometry> geo;
                d2d_->CreatePathGeometry(geo.put());
                Com<ID2D1GeometrySink> sink;
                geo->Open(sink.put());
                sink->SetFillMode(D2D1_FILL_MODE_WINDING);
                OutlineCollector collector(d2d_.p, sink.p, originX, originY);
                layout->Draw(nullptr, &collector, 0.0f, 0.0f);
                sink->Close();
                Com<ID2D1StrokeStyle> style;
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
                d2d_->CreateStrokeStyle(sp, nullptr, 0, style.put());
                rt->FillGeometry(geo.p, brush.p);
                rt->DrawGeometry(geo.p, brush.p, stroke * 2.0f, style.p);
            }
            if (FAILED(rt->EndDraw())) return false;
            WICRect rc{0, 0, raster->width, raster->height};
            std::vector<uint8_t> px4(static_cast<size_t>(raster->width) * static_cast<size_t>(raster->height) * 4);
            if (FAILED(bmp->CopyPixels(&rc, static_cast<UINT>(raster->width * 4), static_cast<UINT>(px4.size()), px4.data())))
                return false;
            mask.resize(static_cast<size_t>(raster->width) * static_cast<size_t>(raster->height));
            for (size_t i = 0; i < mask.size(); ++i) mask[i] = px4[i * 4 + 3];
            return true;
        };
        if (!render(false, raster->fill)) return Result<TextRasterPtr>::error("Direct2D text rendering failed");
        // Visible characters but no ink: the font could not render them (missing
        // glyphs / no usable font). Let the caller fall back to another rasterizer.
        bool anyGlyph = false;
        for (char32_t c : cps)
            if (c != U' ' && c != U'\n' && c != U'\r' && c != U'\t' && c != 0x3000) anyGlyph = true;
        if (anyGlyph && std::none_of(raster->fill.begin(), raster->fill.end(), [](uint8_t v) { return v > 16; }))
            return Result<TextRasterPtr>::error("no glyphs rendered with font family '" + wideToUtf8(family) + "'");
        // Geometric outer stroke (exact, round joins). Wine's Direct2D produces
        // broken outline geometry, so there the compositor derives the stroke
        // by dilating the fill instead (empty stroke mask).
        static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        if (stroke > 0.0f && (wine || !render(true, raster->stroke))) raster->stroke.clear();
        return TextRasterPtr(raster);
    }

    std::vector<std::string> fontFamilies() override {
        std::lock_guard lock(mutex_);
        std::vector<std::string> out;
        Com<IDWriteFontCollection> coll;
        if (FAILED(dw_->GetSystemFontCollection(coll.put(), FALSE))) return out;
        const UINT32 n = coll->GetFontFamilyCount();
        for (UINT32 i = 0; i < n; ++i) {
            Com<IDWriteFontFamily> fam;
            if (FAILED(coll->GetFontFamily(i, fam.put()))) continue;
            Com<IDWriteLocalizedStrings> names;
            if (FAILED(fam->GetFamilyNames(names.put()))) continue;
            UINT32 idx = 0;
            BOOL exists = FALSE;
            names->FindLocaleName(L"en-us", &idx, &exists);
            if (!exists) idx = 0;
            UINT32 len = 0;
            names->GetStringLength(idx, &len);
            std::wstring s(len + 1, L'\0');
            names->GetString(idx, s.data(), len + 1);
            s.resize(len);
            out.push_back(wideToUtf8(s));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string backendName() const override { return "DirectWrite"; }

private:
    // Whether `family` exists and has glyphs for every visible code point.
    bool familyCovers(IDWriteFontCollection* coll, const wchar_t* family, const std::u32string& cps) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (FAILED(coll->FindFamilyName(family, &idx, &exists)) || !exists) return false;
        Com<IDWriteFontFamily> fam;
        Com<IDWriteFont> font;
        Com<IDWriteFontFace> face;
        if (FAILED(coll->GetFontFamily(idx, fam.put())) ||
            FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.put())) ||
            FAILED(font->CreateFontFace(face.put())))
            return false;
        std::vector<UINT32> codes;
        for (char32_t c : cps)
            if (c > 0x20 && c != 0x3000 && c != 0xFEFF) codes.push_back(static_cast<UINT32>(c));
        if (codes.empty()) return true;
        std::vector<UINT16> glyphs(codes.size());
        if (FAILED(face->GetGlyphIndices(codes.data(), static_cast<UINT32>(codes.size()), glyphs.data()))) return false;
        return std::none_of(glyphs.begin(), glyphs.end(), [](UINT16 g) { return g == 0; });
    }

    // Maps the requested family to an installed one that can draw the text
    // (projects may come from machines with other fonts). Returns "" when no
    // installed family covers the text, so the caller can fall back.
    std::wstring resolveFamily(const std::wstring& requested, const std::u32string& cps, bool cjk) {
        Com<IDWriteFontCollection> coll;
        if (FAILED(dw_->GetSystemFontCollection(coll.put(), FALSE)) || !coll) return requested;
        if (familyCovers(coll.p, requested.c_str(), cps)) return requested;
        const std::wstring key = requested + (cjk ? L"|cjk" : L"|latin");
        if (auto it = resolved_.find(key); it != resolved_.end() && (it->second.empty() || familyCovers(coll.p, it->second.c_str(), cps)))
            return it->second;
        static const wchar_t* cjkFaces[] = {L"Yu Gothic UI", L"Yu Gothic", L"Meiryo UI", L"Meiryo", L"MS Gothic", L"MS UI Gothic",
                                            L"BIZ UDGothic", L"Noto Sans CJK JP", L"Noto Sans JP", L"Source Han Sans JP",
                                            L"IPAexGothic", L"IPAGothic", L"Yu Mincho", L"MS Mincho"};
        static const wchar_t* latinFaces[] = {L"Segoe UI", L"Arial", L"Tahoma", L"DejaVu Sans", L"Liberation Sans"};
        std::wstring result;
        for (int pass = 0; pass < 2 && result.empty(); ++pass) {
            const bool useCjk = (pass == 0) == cjk;
            const wchar_t* const* list = useCjk ? cjkFaces : latinFaces;
            const size_t n = useCjk ? std::size(cjkFaces) : std::size(latinFaces);
            for (size_t i = 0; i < n; ++i)
                if (familyCovers(coll.p, list[i], cps)) {
                    result = list[i];
                    break;
                }
        }
        if (result.empty() && !cjk) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(coll->FindFamilyName(requested.c_str(), &idx, &exists)) && exists) result = requested;
        }
        if (result != requested)
            AVC_WARN("text", "font '{}' cannot draw this text here; using '{}'", wideToUtf8(requested),
                     result.empty() ? std::string("fallback rasterizer") : wideToUtf8(result));
        resolved_[key] = result;
        return result;
    }

    std::map<std::wstring, std::wstring> resolved_;
    std::mutex mutex_;
    Com<IDWriteFactory> dw_;
    Com<ID2D1Factory> d2d_;
    Com<IWICImagingFactory> wic_;
};

// Tries DirectWrite first and falls back to stb_truetype per request.
class FallbackRasterizer final : public ITextRasterizer {
public:
    FallbackRasterizer(std::unique_ptr<ITextRasterizer> a, std::unique_ptr<ITextRasterizer> b)
        : primary_(std::move(a)), fallback_(std::move(b)) {}
    Result<TextRasterPtr> rasterize(const TextRequest& r) override {
        auto res = primary_->rasterize(r);
        if (res) return res;
        AVC_WARN("text", "DirectWrite failed ({}); using fallback rasterizer", res.errorMessage());
        return fallback_->rasterize(r);
    }
    std::vector<std::string> fontFamilies() override { return primary_->fontFamilies(); }
    std::string backendName() const override { return primary_->backendName(); }

private:
    std::unique_ptr<ITextRasterizer> primary_, fallback_;
};

}  // namespace

std::unique_ptr<ITextRasterizer> createDirectWriteRasterizer() {
    auto dw = std::make_unique<DirectWriteRasterizer>();
    if (!dw->init()) {
        AVC_WARN("text", "DirectWrite unavailable; using stb_truetype");
        return nullptr;
    }
    return std::make_unique<FallbackRasterizer>(std::move(dw), createStbRasterizer());
}

}  // namespace avc::text
