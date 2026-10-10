// Library: one place to add content. A tab bar along the top switches between
// media, audio (BGM/SFX), text styles, effects, transitions and the AI tools,
// the way modern editors present their asset browser.

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>

#include "core/i18n.h"
#include "ui/panels.h"
#include "ui/text_presets.h"
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

}  // namespace

void LibraryPanel::draw(App& app) {
    UiState& ui = app.ui();
    const float fs = ImGui::GetFontSize();
    // Page bar along the top (icon over label), like the asset browsers of
    // CapCut and other modern editors; the page fills the rest of the panel.
    const int n = static_cast<int>(std::size(kPages));
    const float pad = 6.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float tabW = std::max(fs * 2.6f, (avail - pad * 2) / static_cast<float>(n));
    const float tabH = fs * 3.3f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    for (int i = 0; i < n; ++i) {
        const Page& p = kPages[i];
        if (i > 0) ImGui::SameLine(0, 0);
        if (iconTab(p.id, p.icon, tr(p.label), ui.libraryPage == p.id, ImVec2(tabW, tabH))) ui.libraryPage = p.id;
    }
    const ImVec2 wp = ImGui::GetWindowPos();
    const float lineY = ImGui::GetItemRectMax().y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, lineY), ImVec2(wp.x + ImGui::GetWindowWidth(), lineY), pal::kStroke);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
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
