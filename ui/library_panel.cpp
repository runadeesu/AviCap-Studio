// Library: one place to add content. A vertical icon rail switches between
// media, audio (BGM/SFX), text styles, effects, transitions and the AI tools,
// the way modern editors present their asset browser.

#include <algorithm>
#include <cstdio>
#include <functional>

#include "core/i18n.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

struct Page {
    const char* id;
    Icon icon;
    const char* label;
};

constexpr Page kPages[] = {
    {"media", Icon::Film, trNoop("Media")},           {"audio", Icon::Music, trNoop("Audio")},
    {"text", Icon::Text, trNoop("Text")},              {"effects", Icon::Effects, trNoop("Effects")},
    {"transitions", Icon::Transition, trNoop("Transitions")}, {"ai", Icon::Sparkle, trNoop("AI")},
};

// ------------------------------------------------------------------ text presets

struct TextPreset {
    const char* name;    // English label (tr)
    const char* sample;  // preview text (tr)
    std::function<void(Clip&, const Sequence&)> apply;
    std::function<void(ImDrawList*, ImVec2 a, ImVec2 b, const char* text)> preview;
};

void setColor(Clip& c, const char* k, float r, float g, float b, float a = 1.0f) { c.textParams.setStatic(k, pv(r, g, b, a)); }

void drawCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 a, ImVec2 b, ImU32 col, const char* text, ImVec2 offset = ImVec2(0, 0)) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 p((a.x + b.x - ts.x) * 0.5f + offset.x, (a.y + b.y - ts.y) * 0.5f + offset.y);
    dl->AddText(font, size, p, col, text);
}

void drawStroked(ImDrawList* dl, ImFont* font, float size, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 stroke, float w, const char* text) {
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy) drawCentered(dl, font, size, a, b, stroke, text, ImVec2(static_cast<float>(dx) * w, static_cast<float>(dy) * w));
    drawCentered(dl, font, size, a, b, fill, text);
}

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
             const float size = ImGui::GetFontSize() * 1.1f;
             const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
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

}  // namespace

void LibraryPanel::draw(App& app) {
    UiState& ui = app.ui();
    const float fs = ImGui::GetFontSize();
    const float railW = fs * 4.4f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 6));
    ImGui::BeginChild("##rail", ImVec2(railW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    for (const Page& p : kPages) {
        if (railButton(p.id, p.icon, tr(p.label), ui.libraryPage == p.id, railW)) ui.libraryPage = p.id;
        ImGui::Dummy(ImVec2(0, 2));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // Hairline between the rail and the page.
    const ImVec2 rmax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(rmax.x, ImGui::GetItemRectMin().y), ImVec2(rmax.x, rmax.y), pal::kStroke);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    const char* title = "Media";
    for (const Page& p : kPages)
        if (ui.libraryPage == p.id) title = p.label;
    headingText(tr(title), 1.12f);
    ImGui::Spacing();
    if (ui.libraryPage == "media") media_.draw(app);
    else if (ui.libraryPage == "audio") drawAudio(app);
    else if (ui.libraryPage == "text") drawText(app);
    else if (ui.libraryPage == "effects") effects_.draw(app, EffectsPanel::Mode::Effects);
    else if (ui.libraryPage == "transitions") effects_.draw(app, EffectsPanel::Mode::Transitions);
    else if (ui.libraryPage == "ai") ai_.draw(app);
    else media_.draw(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void LibraryPanel::drawAudio(App& app) {
    // Quick actions first, then the sound library / Myinstants tabs.
    const float half = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    if (iconTextButton("##bgm", Icon::Music, tr("Add Music..."), tr("Choose music files (they go on the music track)"))) app.addAudioDialog(true);
    ImGui::SameLine();
    if (iconTextButton("##sfx", Icon::Sound, tr("Add Sound Effect..."), tr("Choose sound effect files"))) app.addAudioDialog(false);
    (void)half;
    ImGui::Spacing();
    sounds_.draw(app);
}

void LibraryPanel::drawText(App& app) {
    hintText(tr("Click a style to add it at the playhead. Change the words in the Inspector."));
    ImGui::Spacing();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float spacing = 8.0f;
    const int cols = avail > ImGui::GetFontSize() * 18 ? 2 : 1;
    const float w = (avail - spacing * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const float h = std::max(ImGui::GetFontSize() * 4.6f, w * 0.42f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& presets = textPresets();
    // Subtitle card first (most common), then the styles.
    int index = 0;
    auto card = [&](const char* id, const char* name, const std::function<void(ImVec2, ImVec2)>& paint) {
        if (index % cols != 0) ImGui::SameLine(0, spacing);
        ++index;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        dl->AddRectFilled(pos, pos + ImVec2(w, h), hovered ? pal::kPanelHover : pal::kPanelRaised, pal::kControlRadius + 2);
        // Checkerboard-free dark "video" area with the sample.
        const ImVec2 a = pos + ImVec2(6, 6), b = pos + ImVec2(w - 6, h - ImGui::GetTextLineHeight() - 10);
        dl->AddRectFilledMultiColor(a, b, IM_COL32(40, 46, 66, 255), IM_COL32(58, 44, 78, 255), IM_COL32(24, 26, 36, 255), IM_COL32(30, 26, 40, 255));
        dl->PushClipRect(a, b, true);
        paint(a, b);
        dl->PopClipRect();
        dl->AddText(ImVec2(pos.x + 10, b.y + 4), hovered ? pal::kText : pal::kTextMuted, name);
        if (hovered) {
            dl->AddRect(pos, pos + ImVec2(w, h), pal::withAlpha(pal::kAccent, 0.9f), pal::kControlRadius + 2, 1.5f);
            tooltip(tr("Click to add at the playhead"));
        }
        return pressed;
    };
    if (card("##subtitle", tr("Subtitle"), [&](ImVec2 a, ImVec2 b) {
            const char* t = tr("Subtitle text");
            ImFont* f = ImGui::GetFont();
            const float size = ImGui::GetFontSize() * 1.05f;
            const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
            const ImVec2 p((a.x + b.x - ts.x) * 0.5f, b.y - ts.y - 10);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if (dx || dy) dl->AddText(f, size, p + ImVec2(static_cast<float>(dx) * 1.5f, static_cast<float>(dy) * 1.5f), IM_COL32(0, 0, 0, 255), t);
            dl->AddText(f, size, p, IM_COL32(255, 255, 255, 255), t);
        }))
        app.addSubtitleClip(tr("Subtitle"), app.snapToFrame(app.playhead()), Time::fromSeconds(3.0));
    for (size_t i = 0; i < presets.size(); ++i) {
        const TextPreset& p = presets[i];
        const std::string id = "##preset" + std::to_string(i);
        if (card(id.c_str(), tr(p.name), [&](ImVec2 a, ImVec2 b) { p.preview(dl, a, b, tr(p.sample)); }))
            app.addTextClip(tr(p.sample), p.apply);
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (iconTextButton("##importsubs", Icon::Subtitle, tr("Import Subtitles..."), tr("Add subtitles from a .srt or .vtt file")))
        app.importSubtitlesDialog();
    ImGui::SameLine();
    if (iconTextButton("##exportsubs", Icon::Export, tr("Export Subtitles..."), tr("Save the subtitles as a .srt or .vtt file")))
        app.exportSubtitlesDialog();
}

}  // namespace avc::ui
