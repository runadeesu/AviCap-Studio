// Undo history and marker list.

#include <algorithm>
#include <cstring>

#include "core/i18n.h"
#include "timeline/edit_ops.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

void HistoryPanel::draw(App& app) {
    Document& doc = app.doc();
    const auto& undo = doc.undoHistory();
    const auto& redo = doc.redoHistory();
    ImGui::TextDisabled("%zu / %zu %s", undo.size(), doc.undoLimit(), tr("undo steps"));
    ImGui::BeginChild("##history");
    // Oldest first; selecting an entry moves the document to the state after it.
    int target = -2;  // -1 = initial state
    if (ImGui::Selectable(tr("(Initial state)"), undo.empty())) target = -1;
    for (size_t i = 0; i < undo.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(tr(undo[i].label.c_str()), i + 1 == undo.size())) target = static_cast<int>(i);
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (size_t i = redo.size(); i-- > 0;) {
        ImGui::PushID(static_cast<int>(1000000 + i));
        if (ImGui::Selectable(tr(redo[i].label.c_str()))) target = static_cast<int>(undo.size() + (redo.size() - 1 - i));
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    if (target != -2) {
        const int current = static_cast<int>(undo.size()) - 1;
        if (target < current)
            for (int k = current; k > target; --k) app.undo();
        else
            for (int k = current; k < target; ++k) app.redo();
    }
    ImGui::EndChild();
}

void MarkersPanel::draw(App& app) {
    const Sequence* seq = app.sequence();
    if (!seq) return;
    if (ImGui::Button(tr("Add Marker"))) app.addMarker();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s", seq->markers.size(), tr("markers"));
    if (!ImGui::BeginTable("##markers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn(tr("Time"));
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Type"));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    ImGui::TableHeadersRow();
    MarkerId remove = kInvalidId;
    for (const Marker& m : seq->markers) {
        ImGui::PushID(static_cast<int>(m.id));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(formatTime(m.time, seq->frameRate).c_str(), app.selection.marker == m.id, ImGuiSelectableFlags_SpanAllColumns |
                                                                                                          ImGuiSelectableFlags_AllowOverlap)) {
            app.selection.marker = m.id;
            app.seek(m.time);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(m.name.c_str());
        if (!m.comment.empty()) tooltip(m.comment.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", tr(markerKindName(m.kind)));
        ImGui::TableNextColumn();
        if (iconButton("##del", Icon::Close, tr("Delete"), false, ImGui::GetFrameHeight() * 0.8f)) remove = m.id;
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove != kInvalidId)
        app.editSequence("Delete Marker", [&](SequenceEditor& e) {
            return edit::removeMarker(e, remove) ? Status::ok() : Status::error("Marker not found");
        });
}

}  // namespace avc::ui
