#include "ui/text_presets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "core/i18n.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

void setColor(Clip& c, const char* k, float r, float g, float b, float a = 1.0f) { c.textParams.setStatic(k, pv(r, g, b, a)); }

void drawCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 a, ImVec2 b, ImU32 col, const char* text, ImVec2 offset = ImVec2(0, 0)) {
    // Shrink long words (e.g. Japanese labels) so they fit the card.
    const float maxW = (b.x - a.x) - 16.0f;
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    if (ts.x > maxW && ts.x > 0) {
        size *= maxW / ts.x;
        ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    }
    const ImVec2 p((a.x + b.x - ts.x) * 0.5f + offset.x, (a.y + b.y - ts.y) * 0.5f + offset.y);
    dl->AddText(font, size, p, col, text);
}

void drawStroked(ImDrawList* dl, ImFont* font, float size, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 stroke, float w, const char* text) {
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy) drawCentered(dl, font, size, a, b, stroke, text, ImVec2(static_cast<float>(dx) * w, static_cast<float>(dy) * w));
    drawCentered(dl, font, size, a, b, fill, text);
}

}  // namespace

const std::vector<TextPreset>& textPresets() {
    static const std::vector<TextPreset> presets = {
        {trNoop("Title"), trNoop("Title"),
         [](Clip& c, const Sequence& s) {
             c.textStyle.fontSize = static_cast<float>(s.height) * 0.09f;
             c.textStyle.fontWeight = 800;
             setColor(c, "shadowColor", 0, 0, 0, 0.55f);
             c.textParams.setStatic("shadowBlur", pv(c.textStyle.fontSize * 0.12f));
         },
         [](ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t) {
             ImFont* f = boldFont() ? boldFont() : ImGui::GetFont();
             drawCentered(dl, f, ImGui::GetFontSize() * 1.7f, a, b, IM_COL32(0, 0, 0, 120), t, ImVec2(1.5f, 2));
             drawCentered(dl, f, ImGui::GetFontSize() * 1.7f, a, b, IM_COL32(255, 255, 255, 255), t);
         }},
        {trNoop("Pop Caption"), trNoop("Wow!"),
         [](Clip& c, const Sequence& s) {
             c.textStyle.fontSize = static_cast<float>(s.height) * 0.08f;
             c.textStyle.fontWeight = 900;
             setColor(c, "fillColor", 1.0f, 0.86f, 0.12f);
             setColor(c, "strokeColor", 0.05f, 0.05f, 0.08f);
             c.textParams.setStatic("strokeWidth", pv(c.textStyle.fontSize * 0.13f));
             c.transform.setStatic("position", pv(0, static_cast<float>(s.height) * 0.30f));
         },
         [](ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t) {
             ImFont* f = boldFont() ? boldFont() : ImGui::GetFont();
             drawStroked(dl, f, ImGui::GetFontSize() * 1.7f, a, b, IM_COL32(255, 220, 30, 255), IM_COL32(15, 15, 20, 255), 2.5f, t);
         }},
        {trNoop("Lower Third"), trNoop("Name / Title"),
         [](Clip& c, const Sequence& s) {
             c.textStyle.fontSize = static_cast<float>(s.height) * 0.045f;
             c.textStyle.fontWeight = 600;
             c.textStyle.align = TextAlign::Left;
             setColor(c, "backgroundColor", 0.06f, 0.07f, 0.10f, 0.80f);
             c.textParams.setStatic("backgroundPadding", pv(static_cast<float>(s.height) * 0.018f));
             setColor(c, "shadowColor", 0, 0, 0, 0);
             c.transform.setStatic("position", pv(-static_cast<float>(s.width) * 0.26f, static_cast<float>(s.height) * 0.33f));
         },
         [](ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t) {
             ImFont* f = ImGui::GetFont();
             float size = ImGui::GetFontSize() * 1.1f;
             ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
             if (const float room = b.x - a.x - 34; ts.x > room && ts.x > 0) {  // shrink to fit small cards
                 size *= room / ts.x;
                 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
             }
             const ImVec2 p(a.x + 12, b.y - ts.y - 14);
             dl->AddRectFilled(p - ImVec2(8, 5), p + ts + ImVec2(8, 5), IM_COL32(15, 17, 24, 220), 4.0f);
             dl->AddRectFilled(p - ImVec2(8, 5), ImVec2(p.x - 5, p.y + ts.y + 5), pal::kAccent, 2.0f);
             dl->AddText(f, size, p, IM_COL32(255, 255, 255, 255), t);
         }},
        {trNoop("Neon"), trNoop("Neon"),
         [](Clip& c, const Sequence& s) {
             c.textStyle.fontSize = static_cast<float>(s.height) * 0.08f;
             c.textStyle.fontWeight = 700;
             setColor(c, "fillColor", 0.75f, 0.98f, 1.0f);
             setColor(c, "glowColor", 0.1f, 0.75f, 1.0f, 1.0f);
             c.textParams.setStatic("glowRadius", pv(c.textStyle.fontSize * 0.35f));
             setColor(c, "shadowColor", 0, 0, 0, 0);
         },
         [](ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t) {
             ImFont* f = boldFont() ? boldFont() : ImGui::GetFont();
             const float size = ImGui::GetFontSize() * 1.7f;
             for (int r = 4; r >= 1; --r)
                 for (int k = 0; k < 8; ++k) {
                     const float ang = static_cast<float>(k) * 0.785f;
                     drawCentered(dl, f, size, a, b, IM_COL32(30, 190, 255, 28), t,
                                  ImVec2(std::cos(ang) * static_cast<float>(r) * 1.6f, std::sin(ang) * static_cast<float>(r) * 1.6f));
                 }
             drawCentered(dl, f, size, a, b, IM_COL32(200, 250, 255, 255), t);
         }},
        {trNoop("Gradient"), trNoop("Gradient"),
         [](Clip& c, const Sequence& s) {
             c.textStyle.fontSize = static_cast<float>(s.height) * 0.085f;
             c.textStyle.fontWeight = 800;
             c.textParams.setStatic("gradientEnabled", pv(1));
             setColor(c, "fillColor", 1.0f, 0.45f, 0.72f);
             setColor(c, "gradientColor", 0.45f, 0.40f, 1.0f);
         },
         [](ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t) {
             ImFont* f = boldFont() ? boldFont() : ImGui::GetFont();
             const float size = ImGui::GetFontSize() * 1.7f;
             const int vtx0 = dl->VtxBuffer.Size;
             drawCentered(dl, f, size, a, b, IM_COL32(255, 255, 255, 255), t);
             // Recolour the glyph vertices with a vertical gradient.
             const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
             const float y0 = (a.y + b.y - ts.y) * 0.5f, y1 = y0 + ts.y;
             for (int i = vtx0; i < dl->VtxBuffer.Size; ++i) {
                 const float k = std::clamp((dl->VtxBuffer[i].pos.y - y0) / std::max(1.0f, y1 - y0), 0.0f, 1.0f);
                 const ImVec4 top(1.0f, 0.45f, 0.72f, 1.0f), bot(0.45f, 0.40f, 1.0f, 1.0f);
                 const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(top.x + (bot.x - top.x) * k, top.y + (bot.y - top.y) * k, top.z + (bot.z - top.z) * k, 1.0f));
                 const ImU32 a8 = dl->VtxBuffer[i].col & 0xFF000000u;
                 dl->VtxBuffer[i].col = (col & 0x00FFFFFFu) | a8;
             }
         }},
        {trNoop("Simple"), trNoop("Simple text"),
         [](Clip& c, const Sequence& s) {
             c.textStyle.fontSize = static_cast<float>(s.height) * 0.05f;
             c.textStyle.fontWeight = 500;
             setColor(c, "shadowColor", 0, 0, 0, 0.35f);
         },
         [](ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t) {
             drawCentered(dl, ImGui::GetFont(), ImGui::GetFontSize() * 1.25f, a, b, IM_COL32(240, 240, 245, 255), t);
         }},
    };
    return presets;
}


void resetTextLook(Clip& clip) {
    const TextStyle def;
    clip.textStyle.fontSize = def.fontSize;
    clip.textStyle.fontWeight = def.fontWeight;
    clip.textStyle.italic = def.italic;
    clip.textStyle.align = def.align;
    clip.textStyle.tracking = def.tracking;
    clip.textStyle.lineSpacing = def.lineSpacing;
    for (const char* k : {"fillColor", "strokeColor", "strokeWidth", "shadowColor", "shadowOffset", "shadowBlur", "backgroundColor",
                          "backgroundPadding", "glowColor", "glowRadius", "gradientColor", "gradientEnabled"})
        clip.textParams.remove(k);
    clip.transform.remove("position");
}

}  // namespace avc::ui
