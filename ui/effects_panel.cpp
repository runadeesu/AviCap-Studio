// Effects and transitions browser: a searchable grid of tiles with category
// chips. Double-click applies to the selected clips; tiles can be dragged onto
// a clip (effects) or a clip edge (transitions) in the timeline.

#include <algorithm>
#include <cstring>
#include <set>

#include "core/i18n.h"
#include "core/strings.h"
#include "effects/effects.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

bool matches(const std::string& name, const char* filter) {
    if (!filter || !*filter) return true;
    return icontains(name, filter);
}

// Two-colour gradient per category so the grid reads at a glance.
std::pair<ImU32, ImU32> categoryColors(const std::string& cat, bool audio) {
    if (audio) return {IM_COL32(22, 160, 133, 255), IM_COL32(40, 100, 200, 255)};
    if (cat == "Color") return {IM_COL32(255, 128, 64, 255), IM_COL32(236, 72, 153, 255)};
    if (cat == "Blur") return {IM_COL32(56, 132, 255, 255), IM_COL32(34, 211, 238, 255)};
    if (cat == "Distort") return {IM_COL32(139, 92, 246, 255), IM_COL32(79, 70, 229, 255)};
    if (cat == "Keying") return {IM_COL32(34, 197, 94, 255), IM_COL32(20, 184, 166, 255)};
    if (cat == "Mask") return {IM_COL32(100, 116, 139, 255), IM_COL32(71, 85, 105, 255)};
    if (cat == "Stylize") return {IM_COL32(217, 70, 239, 255), IM_COL32(124, 58, 237, 255)};
    if (cat == "Transition") return {IM_COL32(99, 102, 241, 255), IM_COL32(56, 189, 248, 255)};
    if (cat == "Generator") return {IM_COL32(245, 158, 11, 255), IM_COL32(239, 68, 68, 255)};
    return {IM_COL32(71, 85, 105, 255), IM_COL32(51, 65, 85, 255)};
}

struct Tile {
    std::string id;
    std::string label;
    std::string category;
    std::string tip;
    Icon icon = Icon::Effects;
    bool audio = false;
    std::string previewKey;  // AssetService::effectPreview key (empty = icon tile)
};

// Grid of tiles; returns the index double-clicked (or -1). Drag payloads are
// set by the caller through `dragType`.
int tileGrid(App& app, const std::vector<Tile>& tiles, const char* dragType) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float spacing = 8.0f;
    const float minW = ImGui::GetFontSize() * 6.2f;
    const int cols = std::max(2, static_cast<int>((avail + spacing) / (minW + spacing)));
    const float w = (avail - spacing * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const float previewH = w * 0.56f;
    const float labelH = ImGui::GetTextLineHeight() + 6;
    int activated = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Tile& t = tiles[i];
        if (i % static_cast<size_t>(cols) != 0) ImGui::SameLine(0, spacing);
        ImGui::PushID(t.id.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", ImVec2(w, previewH + labelH));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) activated = static_cast<int>(i);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(dragType, t.id.c_str(), t.id.size() + 1);
            ImGui::TextUnformatted(t.label.c_str());
            ImGui::EndDragDropSource();
        }
        tooltip(t.tip.c_str());
        const auto [c0, c1] = categoryColors(t.category, t.audio);
        const ImVec2 a = pos, b = pos + ImVec2(w, previewH);
        const ImTextureID preview = t.previewKey.empty() ? ImTextureID_Invalid : app.assets().effectPreview(t.previewKey);
        if (preview != ImTextureID_Invalid) {
            // The effect applied to the sample picture (cover-fit, rounded).
            const float ar = 192.0f / 108.0f, tileAr = w / previewH;
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (tileAr > ar) {
                const float k = ar / tileAr;
                uv0.y = (1 - k) * 0.5f;
                uv1.y = 1 - uv0.y;
            } else {
                const float k = tileAr / ar;
                uv0.x = (1 - k) * 0.5f;
                uv1.x = 1 - uv0.x;
            }
            dl->AddImageRounded(ImTextureRef(preview), a, b, uv0, uv1, IM_COL32(255, 255, 255, 255), pal::kControlRadius + 1);
            if (!hovered) dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 30), pal::kControlRadius + 1);
        } else {
            // Rounded gradient with the icon.
            gradientRect(dl, a, b, c0, c1, pal::kControlRadius + 1);
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, hovered ? 0 : 40), pal::kControlRadius + 1);
            drawIcon(dl, t.icon, a + ImVec2(w * 0.5f, previewH * 0.5f), previewH * 0.42f, IM_COL32(255, 255, 255, 230));
        }
        if (hovered) dl->AddRect(a, b, IM_COL32(255, 255, 255, 120), pal::kControlRadius + 1, 1.5f);
        // Label (clipped to the tile width).
        ImGui::PushClipRect(ImVec2(pos.x, b.y), ImVec2(pos.x + w, b.y + labelH), true);
        dl->AddText(ImVec2(pos.x + 2, b.y + 4), hovered ? pal::kText : pal::kTextMuted, t.label.c_str());
        ImGui::PopClipRect();
        ImGui::PopID();
    }
    return activated;
}

}  // namespace

void EffectsPanel::draw(App& app) { draw(app, Mode::Effects); }

void EffectsPanel::draw(App& app, Mode mode) {
    const auto& reg = fx::EffectRegistry::instance();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##fxsearch", mode == Mode::Transitions ? tr("Search transitions") : tr("Search effects"), search_, sizeof search_);

    if (mode == Mode::Transitions) {
        hintText(tr("Drag a transition onto the edge between two clips, or double-click to add it to the selected clips."));
        ImGui::BeginChild("##trlist");
        std::vector<Tile> tiles;
        for (const auto& t : fx::transitions()) {
            const std::string shown = tr(t.name.c_str());
            if (!matches(shown, search_) && !matches(t.name, search_)) continue;
            tiles.push_back({t.id, shown, "Transition", tr("Double-click to add at the nearest edge of the selected clips, or drag onto a clip edge"),
                             Icon::Transition, false, "transition:" + t.id});
        }
        const int hit = tileGrid(app, tiles, kPayloadTransition);
        if (hit >= 0)
            if (const auto* t = fx::findTransition(tiles[static_cast<size_t>(hit)].id))
                app.applyTransition(t->id, Time::fromSeconds(t->defaultSeconds));
        ImGui::EndChild();
        return;
    }

    // Category chips.
    std::vector<std::string> cats = {"All"};
    for (const fx::EffectDef* d : reg.list(fx::EffectKind::Video))
        if (std::find(cats.begin(), cats.end(), d->category) == cats.end()) cats.push_back(d->category);
    cats.push_back("Audio");
    cats.push_back("Generator");
    if (std::find(cats.begin(), cats.end(), category_) == cats.end()) category_ = "All";
    for (size_t i = 0; i < cats.size(); ++i) {
        const char* label = tr(cats[i].c_str());
        const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 1.8f;
        if (i > 0) {
            const float next = ImGui::GetItemRectMax().x + 6 + w;
            if (next < ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x) ImGui::SameLine(0, 6);
        }
        if (chipButton(label, category_ == cats[i])) category_ = cats[i];
    }
    ImGui::Spacing();

    ImGui::BeginChild("##fxlist");
    std::vector<Tile> tiles;
    auto addEffects = [&](fx::EffectKind kind) {
        for (const fx::EffectDef* d : reg.list(kind)) {
            const bool audio = kind == fx::EffectKind::Audio;
            if (category_ != "All" && !(audio ? category_ == "Audio" : category_ == d->category)) continue;
            const std::string shown = tr(d->name.c_str());
            if (!matches(shown, search_) && !matches(d->name, search_)) continue;
            const std::string tip = !d->description.empty() ? std::string(tr(d->description.c_str()))
                                                            : std::string(tr("Double-click to apply to the selected clips, or drag onto a clip"));
            tiles.push_back({d->id, shown, d->category, tip, audio ? Icon::Sound : d->category == "Color" ? Icon::Palette : Icon::Effects, audio,
                             audio ? std::string() : d->id});
        }
    };
    addEffects(fx::EffectKind::Video);
    addEffects(fx::EffectKind::Audio);
    if (!tiles.empty()) {
        const int hit = tileGrid(app, tiles, kPayloadEffect);
        if (hit >= 0) app.applyEffect(tiles[static_cast<size_t>(hit)].id);
    }
    // Generators (not effects: they add clips at the playhead).
    if (category_ == "All" || category_ == "Generator") {
        std::vector<Tile> gens;
        if (matches(tr("Adjustment Layer"), search_))
            gens.push_back({"gen.adjustment", tr("Adjustment Layer"), "Generator", tr("Double-click to add at the playhead"), Icon::Effects, false});
        if (matches(tr("Color Matte"), search_))
            gens.push_back({"gen.solid", tr("Color Matte"), "Generator", tr("Double-click to add at the playhead"), Icon::Palette, false});
        if (!gens.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", tr("Generators"));
            const int hit = tileGrid(app, gens, "AVC_GENERATOR");
            if (hit >= 0) {
                if (gens[static_cast<size_t>(hit)].id == "gen.adjustment") app.addAdjustmentLayer();
                else app.addSolidClip(pv(0.1f, 0.1f, 0.1f, 1));
            }
        }
    }
    if (tiles.empty() && category_ != "Generator") ImGui::TextDisabled("%s", tr("No effects match the search."));
    ImGui::EndChild();
}

}  // namespace avc::ui
