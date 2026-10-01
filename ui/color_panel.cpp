// Colour grading for the selected clip: basic correction, lift/gamma/gain/
// offset wheels, curves (master, RGB and hue curves) and 3D LUTs. Each tool
// is a regular effect instance on the clip, so it is keyframable, can be
// reordered in the inspector and renders identically in preview and export.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui_internal.h>

#include "core/i18n.h"
#include "effects/effects.h"
#include "ui/edit_helpers.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

const EffectInstance* findEffect(const Clip& c, const std::string& id) {
    for (const auto& e : c.effects)
        if (e.effectId == id) return &e;
    return nullptr;
}

void addEffect(App& app, ClipId clip, const std::string& id) { app.applyEffectToClip(clip, id); }

std::vector<std::pair<float, float>> parsePoints(const std::string& s, bool centered) {
    std::vector<std::pair<float, float>> pts;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(';', start);
        if (end == std::string::npos) end = s.size();
        const std::string part = s.substr(start, end - start);
        const size_t comma = part.find(',');
        if (comma != std::string::npos)
            pts.emplace_back(std::clamp(std::strtof(part.c_str(), nullptr), 0.0f, 1.0f),
                             std::clamp(std::strtof(part.c_str() + comma + 1, nullptr), 0.0f, 1.0f));
        start = end + 1;
    }
    std::sort(pts.begin(), pts.end());
    if (pts.empty()) pts = centered ? std::vector<std::pair<float, float>>{{0.0f, 0.5f}, {1.0f, 0.5f}}
                                    : std::vector<std::pair<float, float>>{{0.0f, 0.0f}, {1.0f, 1.0f}};
    return pts;
}

std::string formatPoints(const std::vector<std::pair<float, float>>& pts) {
    std::string s;
    char buf[48];
    for (const auto& [x, y] : pts) {
        std::snprintf(buf, sizeof buf, "%s%.4f,%.4f", s.empty() ? "" : ";", x, y);
        s += buf;
    }
    return s;
}

}  // namespace

bool ColorPanel::colorWheel(const char* id, ParamValue& v, float radius) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 c = p0 + ImVec2(radius, radius);
    ImGui::InvisibleButton("##wheel", ImVec2(radius * 2, radius * 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Hue ring
    const int seg = 48;
    for (int i = 0; i < seg; ++i) {
        const float a0 = static_cast<float>(i) / seg * 2 * IM_PI, a1 = static_cast<float>(i + 1) / seg * 2 * IM_PI;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(static_cast<float>(i) / seg, 0.7f, 0.85f, r, g, b);
        const ImU32 colr = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
        dl->PathArcTo(c, radius - 3, a0, a1 + 0.02f, 3);
        dl->PathStroke(colr, 5.0f);
    }
    dl->AddCircleFilled(c, radius - 6, IM_COL32(35, 35, 40, 255), 40);
    dl->AddLine(c - ImVec2(radius - 8, 0), c + ImVec2(radius - 8, 0), IM_COL32(70, 70, 75, 255));
    dl->AddLine(c - ImVec2(0, radius - 8), c + ImVec2(0, radius - 8), IM_COL32(70, 70, 75, 255));
    // Puck: project the zero-sum RGB offset onto the chroma plane.
    const float X = v[0] - 0.5f * (v[1] + v[2]);
    const float Y = 0.8660254f * (v[1] - v[2]);
    const float inner = radius - 8;
    // Hue 0 (red) at angle 0 to the right, counter-clockwise in screen space.
    ImVec2 puck = c + ImVec2(X * inner, -Y * inner);
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const ImVec2 d = ImGui::GetIO().MousePos - c;
        float nx = d.x / inner, ny = -d.y / inner;
        const float len = std::sqrt(nx * nx + ny * ny);
        if (len > 1.0f) {
            nx /= len;
            ny /= len;
        }
        const float slow = ImGui::GetIO().KeyShift ? 0.25f : 1.0f;  // fine adjustment
        nx *= slow;
        ny *= slow;
        v[0] = 2.0f * nx / 3.0f;
        v[1] = -nx / 3.0f + ny / 1.7320508f;
        v[2] = -nx / 3.0f - ny / 1.7320508f;
        puck = c + ImVec2(nx * inner, -ny * inner);
        changed = true;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        v[0] = v[1] = v[2] = 0;
        changed = true;
    }
    dl->AddCircleFilled(puck, 5, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(puck, 6, IM_COL32(0, 0, 0, 255));
    ImGui::PopID();
    return changed;
}

bool ColorPanel::curveEditor(const char* id, std::string& points, ImU32 color, float size, bool centered) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##curve", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p0 + ImVec2(size, size), IM_COL32(25, 25, 28, 255));
    for (int i = 1; i < 4; ++i) {
        const float t = size * static_cast<float>(i) / 4.0f;
        dl->AddLine(p0 + ImVec2(t, 0), p0 + ImVec2(t, size), IM_COL32(50, 50, 55, 255));
        dl->AddLine(p0 + ImVec2(0, t), p0 + ImVec2(size, t), IM_COL32(50, 50, 55, 255));
    }
    if (centered) dl->AddLine(p0 + ImVec2(0, size * 0.5f), p0 + ImVec2(size, size * 0.5f), IM_COL32(90, 90, 95, 255));
    else dl->AddLine(p0 + ImVec2(0, size), p0 + ImVec2(size, 0), IM_COL32(60, 60, 65, 255));
    auto pts = parsePoints(points, centered);
    auto toScreen = [&](float x, float y) { return p0 + ImVec2(x * size, (1.0f - y) * size); };
    const auto table = fx::buildCurveTable(points, 128, centered);
    for (int i = 0; i + 1 < static_cast<int>(table.size()); ++i)
        dl->AddLine(toScreen(static_cast<float>(i) / 127.0f, table[static_cast<size_t>(i)]),
                    toScreen(static_cast<float>(i + 1) / 127.0f, table[static_cast<size_t>(i + 1)]), color, 2.0f);
    bool changed = false;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float mx = std::clamp((m.x - p0.x) / size, 0.0f, 1.0f), my = std::clamp(1.0f - (m.y - p0.y) / size, 0.0f, 1.0f);
    int hovered = -1;
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const ImVec2 sp = toScreen(pts[static_cast<size_t>(i)].first, pts[static_cast<size_t>(i)].second);
        if (std::fabs(sp.x - m.x) < 6 && std::fabs(sp.y - m.y) < 6) hovered = i;
    }
    if (ImGui::IsItemActivated()) {
        if (hovered >= 0) {
            dragPoint_ = hovered;
        } else {
            pts.emplace_back(mx, my);
            std::sort(pts.begin(), pts.end());
            for (int i = 0; i < static_cast<int>(pts.size()); ++i)
                if (pts[static_cast<size_t>(i)].first == mx) dragPoint_ = i;
            changed = true;
        }
    }
    if (ImGui::IsItemActive() && dragPoint_ >= 0 && dragPoint_ < static_cast<int>(pts.size())) {
        const size_t i = static_cast<size_t>(dragPoint_);
        const float lo = i > 0 ? pts[i - 1].first + 0.01f : 0.0f;
        const float hi = i + 1 < pts.size() ? pts[i + 1].first - 0.01f : 1.0f;
        pts[i] = {std::clamp(mx, lo, hi), my};
        changed = true;
    }
    if (ImGui::IsItemDeactivated()) dragPoint_ = -1;
    if (ImGui::IsItemHovered() && hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pts.size() > 2) {
        pts.erase(pts.begin() + hovered);
        changed = true;
    }
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const ImVec2 sp = toScreen(pts[static_cast<size_t>(i)].first, pts[static_cast<size_t>(i)].second);
        dl->AddCircleFilled(sp, i == hovered || i == dragPoint_ ? 5.0f : 4.0f, IM_COL32(240, 240, 240, 255));
    }
    if (changed) points = formatPoints(pts);
    ImGui::PopID();
    return changed;
}

void ColorPanel::draw(App& app) {
    const Sequence* seq = app.sequence();
    const Clip* clip = app.primaryClip();
    const Track* track = clip && seq ? seq->trackOfClip(clip->id) : nullptr;
    if (!clip || !track || !isVisualTrack(track->kind)) {
        ImGui::TextWrapped("%s", tr("Select a video clip (or an adjustment layer) to grade it."));
        return;
    }
    ImGui::TextDisabled("%s", clip->name.c_str());
    const Time local = clipLocalTime(*clip, app.playhead());
    if (ImGui::BeginTabBar("##colortabs")) {
        // ------------------------------------------------ basic
        if (ImGui::BeginTabItem(tr("Basic"))) {
            const EffectInstance* e = findEffect(*clip, "color.basic");
            if (!e) {
                if (ImGui::Button(tr("Add Color Correction"))) addEffect(app, clip->id, "color.basic");
            } else {
                const fx::EffectDef* def = fx::EffectRegistry::instance().find("color.basic");
                for (const ParamDef& d : def->params) {
                    const ParamEdit ed = paramRow(d, e->params.find(d.id), local);
                    applyParamEdit(app, ParamRef{clip->id, ParamTarget::Effect, e->id, d.id}, d, ed, local);
                }
            }
            ImGui::EndTabItem();
        }
        // ------------------------------------------------ wheels
        if (ImGui::BeginTabItem(tr("Wheels"))) {
            const EffectInstance* e = findEffect(*clip, "color.wheels");
            if (!e) {
                if (ImGui::Button(tr("Add Color Wheels"))) addEffect(app, clip->id, "color.wheels");
            } else {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float radius = std::clamp((avail - 30) / 8.0f, 30.0f, 90.0f);
                const char* names[] = {"lift", "gamma", "gain", "offset"};
                const char* labels[] = {"Lift", "Gamma", "Gain", "Offset"};
                for (int i = 0; i < 4; ++i) {
                    if (i > 0) ImGui::SameLine(0, 10);
                    ImGui::BeginGroup();
                    ImGui::TextUnformatted(tr(labels[i]));
                    ParamValue v = e->params.evaluate(names[i], local, pv(0, 0, 0, 0));
                    const ParamRef ref{clip->id, ParamTarget::Effect, e->id, names[i]};
                    const std::string key = std::string("wheel:") + std::to_string(e->id) + names[i];
                    if (colorWheel(names[i], v, radius)) setParamValue(app, ref, v, local, key);
                    ImGui::SetNextItemWidth(radius * 2);
                    float master = v[3];
                    if (ImGui::SliderFloat("##master", &master, -1.0f, 1.0f, "%.2f")) {
                        v[3] = master;
                        setParamValue(app, ref, v, local, key + "m");
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        v[3] = 0;
                        setParamValue(app, ref, v, local, key + "m");
                    }
                    ImGui::EndGroup();
                }
                ImGui::TextDisabled("%s", tr("Drag the puck to tint; Shift for fine control; double-click to reset."));
                if (ImGui::TreeNode(tr("Shadows / Midtones / Highlights"))) {
                    const fx::EffectDef* def = fx::EffectRegistry::instance().find("color.wheels");
                    for (const ParamDef& d : def->params) {
                        if (d.id != "shadows" && d.id != "midtones" && d.id != "highlights") continue;
                        const ParamEdit ed = paramRow(d, e->params.find(d.id), local);
                        applyParamEdit(app, ParamRef{clip->id, ParamTarget::Effect, e->id, d.id}, d, ed, local);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::EndTabItem();
        }
        // ------------------------------------------------ curves
        if (ImGui::BeginTabItem(tr("Curves"))) {
            const EffectInstance* e = findEffect(*clip, "color.curves");
            if (!e) {
                if (ImGui::Button(tr("Add Curves"))) addEffect(app, clip->id, "color.curves");
            } else {
                const char* chans[] = {"master", "red", "green", "blue", "hueHue", "hueSat", "hueLuma", "lumaSat"};
                const char* chanLabels[] = {"Master", "Red", "Green", "Blue", "Hue vs Hue", "Hue vs Sat", "Hue vs Luma", "Luma vs Sat"};
                const ImU32 colors[] = {IM_COL32(230, 230, 230, 255), IM_COL32(240, 80, 80, 255), IM_COL32(80, 220, 100, 255),
                                        IM_COL32(90, 140, 255, 255), IM_COL32(220, 160, 255, 255), IM_COL32(255, 200, 90, 255),
                                        IM_COL32(200, 200, 200, 255), IM_COL32(150, 220, 220, 255)};
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
                const char* chanShown[8];
                for (int i = 0; i < 8; ++i) chanShown[i] = tr(chanLabels[i]);
                ImGui::Combo("##chan", &curveChannel_, chanShown, 8);
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Reset Curve"))) {
                    const ClipId cid = clip->id;
                    const Id eid = e->id;
                    const std::string key = chans[curveChannel_];
                    app.editSequence("Reset Curve", [&](SequenceEditor& ed) {
                        for (auto& ef : ed.mutableClip(cid).effects)
                            if (ef.id == eid) ef.properties.erase(key);
                        return Status::ok();
                    });
                }
                auto it = e->properties.find(chans[curveChannel_]);
                std::string pts = it == e->properties.end() ? std::string() : it->second;
                const float size = std::clamp(ImGui::GetContentRegionAvail().x, 120.0f, 360.0f);
                if (curveEditor("##c", pts, colors[curveChannel_], size, curveChannel_ >= 4)) {
                    const ClipId cid = clip->id;
                    const Id eid = e->id;
                    const std::string key = chans[curveChannel_];
                    app.editSequence("Edit Curve", [&](SequenceEditor& ed) {
                        for (auto& ef : ed.mutableClip(cid).effects)
                            if (ef.id == eid) ef.properties[key] = pts;
                        return Status::ok();
                    }, EditOptions{"curve:" + std::to_string(eid) + key});
                }
                ImGui::TextDisabled("%s", tr("Click to add a point, drag to move, right-click to delete."));
                const fx::EffectDef* def = fx::EffectRegistry::instance().find("color.curves");
                for (const ParamDef& d : def->params) {
                    const ParamEdit ed = paramRow(d, e->params.find(d.id), local);
                    applyParamEdit(app, ParamRef{clip->id, ParamTarget::Effect, e->id, d.id}, d, ed, local);
                }
            }
            ImGui::EndTabItem();
        }
        // ------------------------------------------------ LUT
        if (ImGui::BeginTabItem("LUT")) {
            const EffectInstance* e = findEffect(*clip, "color.lut");
            if (!e) {
                if (ImGui::Button(tr("Add LUT"))) addEffect(app, clip->id, "color.lut");
            } else {
                auto it = e->properties.find("file");
                ImGui::TextWrapped("%s", it == e->properties.end() || it->second.empty() ? tr("No LUT file") : it->second.c_str());
                if (ImGui::Button(tr("Load .cube LUT..."))) {
                    auto files = app.dialogs().openFiles(tr("Load LUT"), {{"3D LUT (.cube)", "*.cube"}}, false);
                    if (!files.empty()) {
                        std::string err;
                        if (!fx::loadCubeLut(files.front(), &err)) {
                            app.notify(LogLevel::Warning, std::string(tr("LUT could not be loaded")) + ": " + err);
                        } else {
                            const ClipId cid = clip->id;
                            const Id eid = e->id;
                            const std::string path = files.front();
                            app.editSequence("Load LUT", [&](SequenceEditor& ed) {
                                for (auto& ef : ed.mutableClip(cid).effects)
                                    if (ef.id == eid) ef.properties["file"] = path;
                                return Status::ok();
                            });
                        }
                    }
                }
                const fx::EffectDef* def = fx::EffectRegistry::instance().find("color.lut");
                for (const ParamDef& d : def->params) {
                    const ParamEdit ed = paramRow(d, e->params.find(d.id), local);
                    applyParamEdit(app, ParamRef{clip->id, ParamTarget::Effect, e->id, d.id}, d, ed, local);
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace avc::ui
