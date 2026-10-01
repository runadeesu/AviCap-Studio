// Effects browser: video effects, transitions and audio effects. Double-click
// applies to the selected clips; drag onto a clip (effects) or a clip edge
// (transitions) in the timeline.

#include <algorithm>
#include <cstring>

#include "core/i18n.h"
#include "effects/effects.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {
bool matches(const std::string& name, const char* filter) {
    if (!filter || !*filter) return true;
    std::string a = name, b = filter;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}
}  // namespace

void EffectsPanel::draw(App& app) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##fxsearch", tr("Search effects"), search_, sizeof search_);
    ImGui::BeginChild("##fxlist");
    const auto& reg = fx::EffectRegistry::instance();

    auto effectList = [&](fx::EffectKind kind, const char* header) {
        if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) return;
        std::string lastCat;
        bool catOpen = false;
        for (const fx::EffectDef* d : reg.list(kind)) {
            const std::string shown = tr(d->name.c_str());
            if (!matches(shown, search_) && !matches(d->name, search_)) continue;
            if (d->category != lastCat) {
                if (catOpen) ImGui::TreePop();
                lastCat = d->category;
                catOpen = ImGui::TreeNodeEx(tr(d->category.c_str()), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            }
            if (!catOpen) continue;
            ImGui::PushID(d->id.c_str());
            ImGui::Selectable(shown.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.applyEffect(d->id);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kPayloadEffect, d->id.c_str(), d->id.size() + 1);
                ImGui::Text("%s", shown.c_str());
                ImGui::EndDragDropSource();
            }
            if (!d->description.empty()) tooltip(tr(d->description.c_str()));
            else tooltip(tr("Double-click to apply to the selected clips, or drag onto a clip"));
            ImGui::PopID();
        }
        if (catOpen) ImGui::TreePop();
    };

    effectList(fx::EffectKind::Video, tr("Video Effects"));
    if (ImGui::CollapsingHeader(tr("Transitions"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& t : fx::transitions()) {
            const std::string shown = tr(t.name.c_str());
            if (!matches(shown, search_) && !matches(t.name, search_)) continue;
            ImGui::PushID(t.id.c_str());
            ImGui::Selectable(shown.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                app.applyTransition(t.id, Time::fromSeconds(t.defaultSeconds));
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kPayloadTransition, t.id.c_str(), t.id.size() + 1);
                ImGui::Text("%s", shown.c_str());
                ImGui::EndDragDropSource();
            }
            tooltip(tr("Double-click to add at the nearest edge of the selected clips, or drag onto a clip edge"));
            ImGui::PopID();
        }
    }
    effectList(fx::EffectKind::Audio, tr("Audio Effects"));
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(tr("Generators"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Selectable(tr("Title"), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            app.addTextClip(tr("Title"));
        tooltip(tr("Double-click to add at the playhead"));
        if (ImGui::Selectable(tr("Subtitle"), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            app.addSubtitleClip(tr("Subtitle"), app.snapToFrame(app.playhead()), Time::fromSeconds(3.0));
        if (ImGui::Selectable(tr("Adjustment Layer"), false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            app.addAdjustmentLayer();
        if (ImGui::Selectable(tr("Color Matte"), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            app.addSolidClip(pv(0.1f, 0.1f, 0.1f, 1));
    }
    ImGui::EndChild();
}

}  // namespace avc::ui
