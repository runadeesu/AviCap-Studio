#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui_internal.h>

#include "core/i18n.h"

namespace avc::ui {

namespace {
bool g_tooltips = true;
std::string g_timeStyle = "timecode";
}  // namespace

ImU32 scaleColor(ImU32 c, float f, float alpha) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.x = std::clamp(v.x * f, 0.0f, 1.0f);
    v.y = std::clamp(v.y * f, 0.0f, 1.0f);
    v.z = std::clamp(v.z * f, 0.0f, 1.0f);
    v.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

ImU32 clipColor(const Clip& c, TrackKind track, MediaKind media) {
    switch (c.kind) {
    case ClipKind::Text: return col::kText;
    case ClipKind::Subtitle: return col::kSubtitle;
    case ClipKind::Solid: return col::kSolid;
    case ClipKind::Adjustment: return col::kAdjustment;
    case ClipKind::Compound: return col::kCompound;
    default: break;
    }
    if (c.colorLabel != 0) return c.colorLabel;
    if (!isVisualTrack(track)) return col::kAudio;
    return media == MediaKind::Image ? col::kImage : col::kVideo;
}

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 color) {
    const float h = s * 0.5f;
    const float t = std::max(1.0f, s * 0.11f);
    switch (icon) {
    case Icon::Play:
        dl->AddTriangleFilled(c + ImVec2(-h * 0.6f, -h * 0.75f), c + ImVec2(h * 0.8f, 0), c + ImVec2(-h * 0.6f, h * 0.75f), color);
        break;
    case Icon::Pause:
        dl->AddRectFilled(c + ImVec2(-h * 0.6f, -h * 0.7f), c + ImVec2(-h * 0.15f, h * 0.7f), color);
        dl->AddRectFilled(c + ImVec2(h * 0.15f, -h * 0.7f), c + ImVec2(h * 0.6f, h * 0.7f), color);
        break;
    case Icon::Stop: dl->AddRectFilled(c + ImVec2(-h * 0.6f, -h * 0.6f), c + ImVec2(h * 0.6f, h * 0.6f), color); break;
    case Icon::ToStart:
        dl->AddRectFilled(c + ImVec2(-h * 0.75f, -h * 0.65f), c + ImVec2(-h * 0.5f, h * 0.65f), color);
        dl->AddTriangleFilled(c + ImVec2(h * 0.6f, -h * 0.65f), c + ImVec2(-h * 0.45f, 0), c + ImVec2(h * 0.6f, h * 0.65f), color);
        break;
    case Icon::ToEnd:
        dl->AddRectFilled(c + ImVec2(h * 0.5f, -h * 0.65f), c + ImVec2(h * 0.75f, h * 0.65f), color);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.6f, -h * 0.65f), c + ImVec2(h * 0.45f, 0), c + ImVec2(-h * 0.6f, h * 0.65f), color);
        break;
    case Icon::StepBack:
        dl->AddTriangleFilled(c + ImVec2(h * 0.1f, -h * 0.6f), c + ImVec2(-h * 0.7f, 0), c + ImVec2(h * 0.1f, h * 0.6f), color);
        dl->AddRectFilled(c + ImVec2(h * 0.3f, -h * 0.6f), c + ImVec2(h * 0.55f, h * 0.6f), color);
        break;
    case Icon::StepForward:
        dl->AddTriangleFilled(c + ImVec2(-h * 0.1f, -h * 0.6f), c + ImVec2(h * 0.7f, 0), c + ImVec2(-h * 0.1f, h * 0.6f), color);
        dl->AddRectFilled(c + ImVec2(-h * 0.55f, -h * 0.6f), c + ImVec2(-h * 0.3f, h * 0.6f), color);
        break;
    case Icon::Loop:
        dl->PathArcTo(c, h * 0.6f, 0.3f, 5.6f, 16);
        dl->PathStroke(color, t);
        dl->AddTriangleFilled(c + ImVec2(h * 0.45f, -h * 0.55f), c + ImVec2(h * 0.95f, -h * 0.2f), c + ImVec2(h * 0.35f, h * 0.05f),
                              color);
        break;
    case Icon::Keyframe:
    case Icon::KeyframeFilled: {
        const ImVec2 p[4] = {c + ImVec2(0, -h * 0.7f), c + ImVec2(h * 0.7f, 0), c + ImVec2(0, h * 0.7f), c + ImVec2(-h * 0.7f, 0)};
        if (icon == Icon::KeyframeFilled) dl->AddConvexPolyFilled(p, 4, color);
        else dl->AddPolyline(p, 4, color, t, ImDrawFlags_Closed);
        break;
    }
    case Icon::KeyPrev:
        dl->AddTriangleFilled(c + ImVec2(h * 0.4f, -h * 0.55f), c + ImVec2(-h * 0.5f, 0), c + ImVec2(h * 0.4f, h * 0.55f), color);
        break;
    case Icon::KeyNext:
        dl->AddTriangleFilled(c + ImVec2(-h * 0.4f, -h * 0.55f), c + ImVec2(h * 0.5f, 0), c + ImVec2(-h * 0.4f, h * 0.55f), color);
        break;
    case Icon::Plus:
        dl->AddLine(c + ImVec2(-h * 0.6f, 0), c + ImVec2(h * 0.6f, 0), color, t * 1.3f);
        dl->AddLine(c + ImVec2(0, -h * 0.6f), c + ImVec2(0, h * 0.6f), color, t * 1.3f);
        break;
    case Icon::Minus: dl->AddLine(c + ImVec2(-h * 0.6f, 0), c + ImVec2(h * 0.6f, 0), color, t * 1.3f); break;
    case Icon::Close:
        dl->AddLine(c + ImVec2(-h * 0.5f, -h * 0.5f), c + ImVec2(h * 0.5f, h * 0.5f), color, t * 1.2f);
        dl->AddLine(c + ImVec2(h * 0.5f, -h * 0.5f), c + ImVec2(-h * 0.5f, h * 0.5f), color, t * 1.2f);
        break;
    case Icon::Eye:
    case Icon::EyeOff:
        dl->AddEllipse(c, ImVec2(h * 0.8f, h * 0.45f), color, 0, 20, t);
        dl->AddCircleFilled(c, h * 0.22f, color);
        if (icon == Icon::EyeOff) dl->AddLine(c + ImVec2(-h * 0.8f, h * 0.7f), c + ImVec2(h * 0.8f, -h * 0.7f), color, t * 1.3f);
        break;
    case Icon::Lock:
    case Icon::Unlock:
        dl->AddRectFilled(c + ImVec2(-h * 0.55f, -h * 0.05f), c + ImVec2(h * 0.55f, h * 0.7f), color, 1.5f);
        dl->PathArcTo(c + ImVec2(icon == Icon::Unlock ? h * 0.35f : 0, -h * 0.1f), h * 0.35f, IM_PI, 2 * IM_PI, 10);
        dl->PathStroke(color, t * 1.2f);
        break;
    case Icon::Speaker:
    case Icon::Mute:
        dl->AddRectFilled(c + ImVec2(-h * 0.7f, -h * 0.25f), c + ImVec2(-h * 0.35f, h * 0.25f), color);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.4f, -h * 0.25f), c + ImVec2(h * 0.1f, -h * 0.7f), c + ImVec2(h * 0.1f, h * 0.7f), color);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.4f, -h * 0.25f), c + ImVec2(h * 0.1f, h * 0.7f), c + ImVec2(-h * 0.4f, h * 0.25f), color);
        if (icon == Icon::Mute) {
            dl->AddLine(c + ImVec2(h * 0.3f, -h * 0.3f), c + ImVec2(h * 0.8f, h * 0.3f), color, t);
            dl->AddLine(c + ImVec2(h * 0.8f, -h * 0.3f), c + ImVec2(h * 0.3f, h * 0.3f), color, t);
        } else {
            dl->PathArcTo(c + ImVec2(h * 0.1f, 0), h * 0.5f, -0.8f, 0.8f, 8);
            dl->PathStroke(color, t);
        }
        break;
    case Icon::Razor:
        dl->AddLine(c + ImVec2(-h * 0.1f, -h * 0.8f), c + ImVec2(-h * 0.1f, h * 0.8f), color, t);
        dl->AddTriangleFilled(c + ImVec2(h * 0.05f, -h * 0.6f), c + ImVec2(h * 0.7f, -h * 0.2f), c + ImVec2(h * 0.05f, h * 0.2f), color);
        break;
    case Icon::Pointer: {
        const ImVec2 p[3] = {c + ImVec2(-h * 0.5f, -h * 0.8f), c + ImVec2(h * 0.55f, h * 0.15f), c + ImVec2(-h * 0.5f, h * 0.55f)};
        dl->AddConvexPolyFilled(p, 3, color);
        dl->AddLine(c + ImVec2(-h * 0.05f, h * 0.3f), c + ImVec2(h * 0.25f, h * 0.85f), color, t * 1.5f);
        break;
    }
    case Icon::Magnet:
        dl->PathArcTo(c + ImVec2(0, -h * 0.05f), h * 0.55f, IM_PI, 2 * IM_PI, 12);
        dl->PathStroke(color, t * 2.2f);
        dl->AddLine(c + ImVec2(-h * 0.55f, -h * 0.05f), c + ImVec2(-h * 0.55f, h * 0.7f), color, t * 2.2f);
        dl->AddLine(c + ImVec2(h * 0.55f, -h * 0.05f), c + ImVec2(h * 0.55f, h * 0.7f), color, t * 2.2f);
        break;
    case Icon::Link:
        dl->AddEllipse(c + ImVec2(-h * 0.3f, 0), ImVec2(h * 0.45f, h * 0.28f), color, 0, 16, t);
        dl->AddEllipse(c + ImVec2(h * 0.3f, 0), ImVec2(h * 0.45f, h * 0.28f), color, 0, 16, t);
        break;
    case Icon::Gear:
        dl->AddCircle(c, h * 0.45f, color, 16, t * 2.0f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * IM_PI / 4.0f;
            dl->AddLine(c + ImVec2(std::cos(a), std::sin(a)) * (h * 0.5f), c + ImVec2(std::cos(a), std::sin(a)) * (h * 0.85f), color,
                        t * 1.8f);
        }
        break;
    case Icon::Folder:
        dl->AddRectFilled(c + ImVec2(-h * 0.8f, -h * 0.45f), c + ImVec2(-h * 0.1f, -h * 0.2f), color, 1.0f);
        dl->AddRectFilled(c + ImVec2(-h * 0.8f, -h * 0.3f), c + ImVec2(h * 0.8f, h * 0.6f), color, 1.5f);
        break;
    case Icon::Trash:
        dl->AddRectFilled(c + ImVec2(-h * 0.7f, -h * 0.6f), c + ImVec2(h * 0.7f, -h * 0.4f), color);
        dl->AddRectFilled(c + ImVec2(-h * 0.5f, -h * 0.35f), c + ImVec2(h * 0.5f, h * 0.75f), color, 1.5f);
        break;
    case Icon::Marker: {
        const ImVec2 p[5] = {c + ImVec2(-h * 0.5f, -h * 0.7f), c + ImVec2(h * 0.5f, -h * 0.7f), c + ImVec2(h * 0.5f, h * 0.2f),
                             c + ImVec2(0, h * 0.75f), c + ImVec2(-h * 0.5f, h * 0.2f)};
        dl->AddConvexPolyFilled(p, 5, color);
        break;
    }
    case Icon::Up:
        dl->AddTriangleFilled(c + ImVec2(0, -h * 0.5f), c + ImVec2(h * 0.6f, h * 0.35f), c + ImVec2(-h * 0.6f, h * 0.35f), color);
        break;
    case Icon::Down:
        dl->AddTriangleFilled(c + ImVec2(-h * 0.6f, -h * 0.35f), c + ImVec2(h * 0.6f, -h * 0.35f), c + ImVec2(0, h * 0.5f), color);
        break;
    case Icon::Reset:
        dl->PathArcTo(c, h * 0.55f, -2.4f, 2.6f, 16);
        dl->PathStroke(color, t * 1.2f);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.75f, -h * 0.65f), c + ImVec2(-h * 0.1f, -h * 0.55f), c + ImVec2(-h * 0.55f, -h * 0.05f),
                              color);
        break;
    case Icon::Search:
        dl->AddCircle(c + ImVec2(-h * 0.15f, -h * 0.15f), h * 0.45f, color, 16, t * 1.3f);
        dl->AddLine(c + ImVec2(h * 0.2f, h * 0.2f), c + ImVec2(h * 0.7f, h * 0.7f), color, t * 2.0f);
        break;
    case Icon::Film:
        dl->AddRectFilled(c + ImVec2(-h * 0.8f, -h * 0.55f), c + ImVec2(h * 0.8f, h * 0.55f), color, 1.5f);
        for (int i = -2; i <= 2; ++i) {
            const float x = static_cast<float>(i) * h * 0.32f;
            dl->AddRectFilled(c + ImVec2(x - h * 0.08f, -h * 0.45f), c + ImVec2(x + h * 0.08f, -h * 0.3f), IM_COL32(0, 0, 0, 160));
            dl->AddRectFilled(c + ImVec2(x - h * 0.08f, h * 0.3f), c + ImVec2(x + h * 0.08f, h * 0.45f), IM_COL32(0, 0, 0, 160));
        }
        break;
    case Icon::Music:
        dl->AddCircleFilled(c + ImVec2(-h * 0.35f, h * 0.45f), h * 0.25f, color);
        dl->AddLine(c + ImVec2(-h * 0.12f, h * 0.45f), c + ImVec2(-h * 0.12f, -h * 0.7f), color, t * 1.3f);
        dl->AddLine(c + ImVec2(-h * 0.12f, -h * 0.7f), c + ImVec2(h * 0.5f, -h * 0.45f), color, t * 2.0f);
        break;
    case Icon::Image:
        dl->AddRect(c + ImVec2(-h * 0.8f, -h * 0.6f), c + ImVec2(h * 0.8f, h * 0.6f), color, 1.5f, t);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.6f, h * 0.45f), c + ImVec2(-h * 0.1f, -h * 0.15f), c + ImVec2(h * 0.35f, h * 0.45f), color);
        dl->AddCircleFilled(c + ImVec2(h * 0.4f, -h * 0.25f), h * 0.13f, color);
        break;
    case Icon::Text:
        dl->AddLine(c + ImVec2(-h * 0.6f, -h * 0.6f), c + ImVec2(h * 0.6f, -h * 0.6f), color, t * 1.8f);
        dl->AddLine(c + ImVec2(0, -h * 0.6f), c + ImVec2(0, h * 0.7f), color, t * 1.8f);
        break;
    case Icon::Check:
        dl->AddLine(c + ImVec2(-h * 0.6f, 0), c + ImVec2(-h * 0.15f, h * 0.5f), color, t * 1.8f);
        dl->AddLine(c + ImVec2(-h * 0.15f, h * 0.5f), c + ImVec2(h * 0.65f, -h * 0.5f), color, t * 1.8f);
        break;
    case Icon::Save:
        dl->AddRect(c + ImVec2(-h * 0.7f, -h * 0.7f), c + ImVec2(h * 0.7f, h * 0.7f), color, 2.0f, t * 1.2f);
        dl->AddRectFilled(c + ImVec2(-h * 0.4f, -h * 0.7f), c + ImVec2(h * 0.35f, -h * 0.2f), color);
        dl->AddRectFilled(c + ImVec2(-h * 0.45f, h * 0.15f), c + ImVec2(h * 0.45f, h * 0.7f), color, 1.0f);
        break;
    case Icon::Undo:
    case Icon::Redo: {
        const float dir = icon == Icon::Undo ? 1.0f : -1.0f;
        dl->PathArcTo(c + ImVec2(0, h * 0.15f), h * 0.5f, icon == Icon::Undo ? -2.6f : -0.55f, icon == Icon::Undo ? 0.6f : 2.6f + 0.0f, 14);
        dl->PathStroke(color, t * 1.4f);
        const ImVec2 tip = c + ImVec2(-dir * h * 0.42f, -h * 0.2f);
        dl->AddTriangleFilled(tip + ImVec2(-dir * h * 0.32f, -h * 0.05f), tip + ImVec2(dir * h * 0.12f, -h * 0.38f),
                              tip + ImVec2(dir * h * 0.15f, h * 0.2f), color);
        break;
    }
    case Icon::Export:
        dl->AddRect(c + ImVec2(-h * 0.7f, -h * 0.25f), c + ImVec2(h * 0.7f, h * 0.7f), color, 2.0f, t * 1.2f);
        dl->AddLine(c + ImVec2(0, h * 0.35f), c + ImVec2(0, -h * 0.7f), color, t * 1.6f);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.35f, -h * 0.4f), c + ImVec2(0, -h * 0.85f), c + ImVec2(h * 0.35f, -h * 0.4f), color);
        break;
    case Icon::Subtitle:
        dl->AddRect(c + ImVec2(-h * 0.8f, -h * 0.55f), c + ImVec2(h * 0.8f, h * 0.55f), color, 2.0f, t * 1.2f);
        dl->AddLine(c + ImVec2(-h * 0.5f, h * 0.05f), c + ImVec2(h * 0.1f, h * 0.05f), color, t * 1.5f);
        dl->AddLine(c + ImVec2(h * 0.25f, h * 0.05f), c + ImVec2(h * 0.5f, h * 0.05f), color, t * 1.5f);
        dl->AddLine(c + ImVec2(-h * 0.5f, h * 0.3f), c + ImVec2(-h * 0.2f, h * 0.3f), color, t * 1.5f);
        dl->AddLine(c + ImVec2(-h * 0.05f, h * 0.3f), c + ImVec2(h * 0.5f, h * 0.3f), color, t * 1.5f);
        break;
    case Icon::Effects:
        dl->AddLine(c + ImVec2(-h * 0.7f, h * 0.7f), c + ImVec2(h * 0.2f, -h * 0.2f), color, t * 2.0f);
        for (int i = 0; i < 4; ++i) {
            const float a = static_cast<float>(i) * IM_PI / 2.0f;
            const ImVec2 sc = c + ImVec2(h * 0.4f, -h * 0.4f);
            dl->AddLine(sc, sc + ImVec2(std::cos(a), std::sin(a)) * (h * 0.32f), color, t * 1.3f);
        }
        break;
    case Icon::Palette:
        dl->AddCircle(c, h * 0.72f, color, 20, t * 1.3f);
        dl->AddCircleFilled(c + ImVec2(-h * 0.3f, -h * 0.25f), h * 0.15f, IM_COL32(240, 80, 80, 255));
        dl->AddCircleFilled(c + ImVec2(h * 0.15f, -h * 0.38f), h * 0.15f, IM_COL32(90, 210, 110, 255));
        dl->AddCircleFilled(c + ImVec2(h * 0.38f, h * 0.05f), h * 0.15f, IM_COL32(90, 140, 255, 255));
        dl->AddCircleFilled(c + ImVec2(-h * 0.2f, h * 0.3f), h * 0.15f, IM_COL32(240, 210, 70, 255));
        break;
    case Icon::Help:
        dl->AddCircle(c, h * 0.72f, color, 20, t * 1.3f);
        dl->PathArcTo(c + ImVec2(0, -h * 0.18f), h * 0.25f, -IM_PI, 0.5f, 10);
        dl->PathStroke(color, t * 1.4f);
        dl->AddLine(c + ImVec2(h * 0.05f, h * 0.0f), c + ImVec2(0, h * 0.22f), color, t * 1.4f);
        dl->AddCircleFilled(c + ImVec2(0, h * 0.45f), t * 0.9f, color);
        break;
    case Icon::Sound:
        dl->AddRectFilled(c + ImVec2(-h * 0.7f, -h * 0.2f), c + ImVec2(-h * 0.45f, h * 0.2f), color);
        dl->AddRectFilled(c + ImVec2(-h * 0.35f, -h * 0.55f), c + ImVec2(-h * 0.1f, h * 0.55f), color);
        dl->AddRectFilled(c + ImVec2(0.0f, -h * 0.35f), c + ImVec2(h * 0.25f, h * 0.35f), color);
        dl->AddRectFilled(c + ImVec2(h * 0.35f, -h * 0.7f), c + ImVec2(h * 0.6f, h * 0.7f), color);
        break;
    case Icon::Warning:
        dl->AddTriangleFilled(c + ImVec2(0, -h * 0.8f), c + ImVec2(h * 0.85f, h * 0.65f), c + ImVec2(-h * 0.85f, h * 0.65f), color);
        dl->AddLine(c + ImVec2(0, -h * 0.3f), c + ImVec2(0, h * 0.2f), IM_COL32(0, 0, 0, 255), t * 1.5f);
        dl->AddCircleFilled(c + ImVec2(0, h * 0.42f), t, IM_COL32(0, 0, 0, 255));
        break;
    }
}

bool iconButton(const char* id, Icon icon, const char* tip, bool active, float size) {
    const float s = size > 0 ? size : ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    ImU32 bg = active ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                      : hovered ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : ImGui::GetColorU32(ImGuiCol_Button);
    dl->AddRectFilled(pos, pos + ImVec2(s, s), bg, st.FrameRounding);
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text, active ? 1.0f : 0.85f);
    drawIcon(dl, icon, pos + ImVec2(s * 0.5f, s * 0.5f), s * 0.62f, fg);
    if (tip) tooltip(tip);
    return pressed;
}

bool textToggle(const char* label, bool* value, const char* tip, ImU32 onColor) {
    const bool on = *value;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, onColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scaleColor(onColor, 1.15f));
    }
    const bool pressed = ImGui::SmallButton(label);
    if (on) ImGui::PopStyleColor(2);
    if (pressed) *value = !*value;
    if (tip) tooltip(tip);
    return pressed;
}

bool iconTextButton(const char* id, Icon icon, const char* label, const char* tip, bool active, bool enabled) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float iconW = h * 0.9f;
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size(iconW + textSize.x + st.FramePadding.x * 2.0f + 2.0f, h);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = active ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                            : (hovered && enabled) ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : ImGui::GetColorU32(ImGuiCol_Button, 0.55f);
    dl->AddRectFilled(pos, pos + size, bg, st.FrameRounding);
    const ImU32 fg = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    drawIcon(dl, icon, pos + ImVec2(st.FramePadding.x + iconW * 0.45f, h * 0.5f), h * 0.55f, fg);
    dl->AddText(pos + ImVec2(st.FramePadding.x + iconW, (h - textSize.y) * 0.5f), fg, label, ImGui::FindRenderedTextEnd(label));
    ImGui::EndDisabled();
    if (tip) tooltip(tip);
    return pressed && enabled;
}

void formLabel(const char* label, float labelWidth) {
    if (labelWidth <= 0) labelWidth = ImGui::GetFontSize() * 9.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-1);
}

const char* formRow(const char* label, float labelWidth) {
    static thread_local std::string ids[4];
    static thread_local int next = 0;
    formLabel(label, labelWidth);
    std::string& id = ids[next++ & 3];
    id = std::string("##") + label;
    return id.c_str();
}

void setTooltipsEnabled(bool on) { g_tooltips = on; }

void tooltip(const char* text) {
    if (!g_tooltips || !text || !*text) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ------------------------------------------------------------------ time

void setTimeStyle(const std::string& style) { g_timeStyle = style; }

std::string formatTimeAs(Time t, Rational fps, const std::string& style) {
    if (style == "frames") return std::to_string(t.toFrames(fps, Rounding::Nearest));
    if (style == "seconds") {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.3f s", t.seconds());
        return buf;
    }
    return formatTimecode(t, fps);
}

std::string formatTime(Time t, Rational fps) { return formatTimeAs(t, fps, g_timeStyle); }

bool timeField(const char* id, Time& t, Rational fps, float width) {
    char buf[64];
    const std::string shown = formatTime(t, fps);
    std::snprintf(buf, sizeof buf, "%s", shown.c_str());
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    if (ImGui::InputText(id, buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        std::string s = buf;
        std::optional<Time> parsed;
        if (s.find(':') != std::string::npos || s.find(';') != std::string::npos) {
            parsed = parseTimecode(s, fps);
        } else if (!s.empty()) {
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            const bool secs = end && (*end == 's' || (*end == ' ' && end[1] == 's') || std::strchr(s.c_str(), '.'));
            if (end != s.c_str()) parsed = secs ? Time::fromSeconds(v) : Time::fromFrames(static_cast<int64_t>(v), fps);
        }
        if (parsed && parsed->ticks >= 0) {
            t = *parsed;
            changed = true;
        }
    }
    return changed;
}

// ------------------------------------------------------------------ meters

float linearToDb(float v) { return v <= 1e-6f ? -120.0f : 20.0f * std::log10(v); }

void levelMeter(const char* id, const audio::Meter& m, ImVec2 size, bool vertical) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, IM_COL32(18, 18, 20, 255));
    auto norm = [](float lin) { return std::clamp((linearToDb(lin) + 60.0f) / 66.0f, 0.0f, 1.0f); };
    for (int ch = 0; ch < 2; ++ch) {
        const float pk = norm(m.peak[ch]), rms = norm(m.rms[ch]);
        if (vertical) {
            const float w = (size.x - 3) * 0.5f;
            const float x0 = pos.x + 1 + static_cast<float>(ch) * (w + 1);
            auto y = [&](float n) { return pos.y + size.y - n * size.y; };
            dl->AddRectFilledMultiColor(ImVec2(x0, y(rms)), ImVec2(x0 + w, pos.y + size.y), IM_COL32(60, 200, 90, 255),
                                        IM_COL32(60, 200, 90, 255), IM_COL32(40, 140, 70, 255), IM_COL32(40, 140, 70, 255));
            const float yp = y(pk);
            const ImU32 pc = pk > norm(0.89f) ? IM_COL32(240, 70, 50, 255) : pk > norm(0.5f) ? IM_COL32(240, 200, 60, 255)
                                                                                         : IM_COL32(120, 230, 140, 255);
            dl->AddLine(ImVec2(x0, yp), ImVec2(x0 + w, yp), pc, 2.0f);
        } else {
            const float hgt = (size.y - 3) * 0.5f;
            const float y0 = pos.y + 1 + static_cast<float>(ch) * (hgt + 1);
            dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(pos.x + rms * size.x, y0 + hgt), IM_COL32(60, 200, 90, 255));
            const float xp = pos.x + pk * size.x;
            dl->AddLine(ImVec2(xp, y0), ImVec2(xp, y0 + hgt), IM_COL32(240, 220, 120, 255), 2.0f);
        }
    }
    if (m.clipped) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + 4), IM_COL32(240, 50, 40, 255));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("L %.1f dB  R %.1f dB", linearToDb(m.peak[0]), linearToDb(m.peak[1]));
    }
    (void)id;
}

// ------------------------------------------------------------------ parameters

bool sectionHeader(const char* label, bool defaultOpen) {
    return ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

ParamEdit paramRow(const ParamDef& def, const AnimatedParam* param, Time local, bool showKeyframes) {
    ParamEdit out;
    ImGui::PushID(def.id.c_str());
    const ParamValue current = param ? param->evaluate(local) : def.def;
    const bool animated = param && param->animated();
    const float fh = ImGui::GetFrameHeight();

    if (showKeyframes && def.keyframable) {
        if (iconButton("##anim", animated ? Icon::KeyframeFilled : Icon::Keyframe,
                       animated ? tr("Disable keyframes") : tr("Enable keyframes"), animated, fh * 0.8f))
            out.toggleAnimation = true;
        ImGui::SameLine(0, 2);
    } else {
        ImGui::Dummy(ImVec2(fh * 0.8f, fh * 0.8f));
        ImGui::SameLine(0, 2);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(def.label.c_str()));
    const float labelW = ImGui::GetFontSize() * 8.5f;
    ImGui::SameLine(labelW);
    const float right = animated ? fh * 3.4f : fh * 1.2f;
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - right));
    ParamValue v = current;
    const char* unit = def.unit.c_str();
    char fmt[32];
    const int decimals = def.step >= 1.0f ? 0 : def.step >= 0.1f ? 1 : 2;
    std::snprintf(fmt, sizeof fmt, "%%.%df%s%s", decimals, def.unit.empty() ? "" : " ", unit);
    bool edited = false;
    switch (def.type) {
    case ParamType::Vec2: {
        float xy[2] = {v[0], v[1]};
        edited = ImGui::DragFloat2("##v", xy, def.step, def.minValue, def.maxValue, fmt);
        v[0] = xy[0];
        v[1] = xy[1];
        break;
    }
    case ParamType::Color: {
        float rgba[4] = {v[0], v[1], v[2], v[3]};
        edited = ImGui::ColorEdit4("##v", rgba, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float);
        v = {rgba[0], rgba[1], rgba[2], rgba[3]};
        break;
    }
    case ParamType::Bool: {
        bool b = v[0] >= 0.5f;
        edited = ImGui::Checkbox("##v", &b);
        v[0] = b ? 1.0f : 0.0f;
        break;
    }
    case ParamType::Enum: {
        const int count = static_cast<int>(def.enumLabels.size());
        int idx = std::clamp(static_cast<int>(std::lround(v[0])), 0, std::max(0, count - 1));
        const char* preview = count ? tr(def.enumLabels[static_cast<size_t>(idx)].c_str()) : "";
        if (ImGui::BeginCombo("##v", preview)) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(tr(def.enumLabels[static_cast<size_t>(i)].c_str()), i == idx)) {
                    idx = i;
                    edited = true;
                }
            ImGui::EndCombo();
        }
        v[0] = static_cast<float>(idx);
        break;
    }
    case ParamType::Int: {
        int iv = static_cast<int>(std::lround(v[0]));
        edited = ImGui::DragInt("##v", &iv, std::max(0.2f, def.step), static_cast<int>(def.minValue), static_cast<int>(def.maxValue));
        v[0] = static_cast<float>(iv);
        break;
    }
    default:
        edited = ImGui::DragFloat("##v", &v[0], def.step, def.minValue, def.maxValue, fmt);
        break;
    }
    out.editing = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Right)) out.reset = true;
    if (edited) {
        out.changed = true;
        out.value = v;
    }
    if (animated) {
        const auto& keys = param->keys();
        const int at = param->keyIndexAt(local, Time::fromMilliseconds(1));
        ImGui::SameLine(0, 2);
        if (iconButton("##prev", Icon::KeyPrev, tr("Previous keyframe"), false, fh * 0.8f)) {
            for (auto it = keys.rbegin(); it != keys.rend(); ++it)
                if (it->time < local - Time::fromMilliseconds(1)) {
                    out.jumpTo = it->time;
                    break;
                }
        }
        ImGui::SameLine(0, 1);
        if (iconButton("##key", at >= 0 ? Icon::KeyframeFilled : Icon::Keyframe,
                       at >= 0 ? tr("Remove keyframe") : tr("Add keyframe"), false, fh * 0.8f)) {
            if (at >= 0) out.removeKey = true;
            else out.addKey = true;
        }
        ImGui::SameLine(0, 1);
        if (iconButton("##next", Icon::KeyNext, tr("Next keyframe"), false, fh * 0.8f)) {
            for (const auto& k : keys)
                if (k.time > local + Time::fromMilliseconds(1)) {
                    out.jumpTo = k.time;
                    break;
                }
        }
    } else {
        ImGui::SameLine(0, 2);
        if (iconButton("##reset", Icon::Reset, tr("Reset to default"), false, fh * 0.8f)) out.reset = true;
    }
    ImGui::PopID();
    return out;
}

}  // namespace avc::ui
