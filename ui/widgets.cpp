#include "ui/widgets.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
    case Icon::Sparkle: {
        // Four-point star plus a small one: the usual "AI" glyph.
        auto star = [&](ImVec2 o, float r) {
            const ImVec2 p[8] = {o + ImVec2(0, -r), o + ImVec2(r * 0.22f, -r * 0.22f), o + ImVec2(r, 0), o + ImVec2(r * 0.22f, r * 0.22f),
                                 o + ImVec2(0, r), o + ImVec2(-r * 0.22f, r * 0.22f), o + ImVec2(-r, 0), o + ImVec2(-r * 0.22f, -r * 0.22f)};
            dl->AddConcavePolyFilled(p, 8, color);
        };
        star(c + ImVec2(-h * 0.12f, h * 0.1f), h * 0.72f);
        star(c + ImVec2(h * 0.6f, -h * 0.6f), h * 0.3f);
        break;
    }
    case Icon::Transition:
        dl->AddRectFilled(c + ImVec2(-h * 0.85f, -h * 0.55f), c + ImVec2(-h * 0.05f, h * 0.55f), color, 2.0f);
        dl->AddRect(c + ImVec2(h * 0.05f, -h * 0.55f), c + ImVec2(h * 0.85f, h * 0.55f), color, 2.0f, t * 1.2f);
        dl->AddTriangleFilled(c + ImVec2(-h * 0.25f, -h * 0.3f), c + ImVec2(h * 0.25f, 0), c + ImVec2(-h * 0.25f, h * 0.3f), color);
        break;
    case Icon::Grid:
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                const ImVec2 o = c + ImVec2((i ? 0.1f : -0.75f) * h, (j ? 0.1f : -0.75f) * h);
                dl->AddRectFilled(o, o + ImVec2(h * 0.65f, h * 0.65f), color, 1.5f);
            }
        break;
    case Icon::List:
        for (int i = -1; i <= 1; ++i) {
            const float y = static_cast<float>(i) * h * 0.5f;
            dl->AddCircleFilled(c + ImVec2(-h * 0.65f, y), t * 1.1f, color);
            dl->AddLine(c + ImVec2(-h * 0.35f, y), c + ImVec2(h * 0.75f, y), color, t * 1.4f);
        }
        break;
    case Icon::Fullscreen: {
        const float a = h * 0.75f, b = h * 0.3f;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                const ImVec2 corner = c + ImVec2(sx * a, sy * a);
                dl->AddLine(corner, corner - ImVec2(sx * b, 0), color, t * 1.4f);
                dl->AddLine(corner, corner - ImVec2(0, sy * b), color, t * 1.4f);
            }
        break;
    }
    case Icon::Star:
    case Icon::StarFilled: {
        ImVec2 p[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -IM_PI / 2 + static_cast<float>(i) * IM_PI / 5.0f;
            const float r = (i % 2 == 0) ? h * 0.8f : h * 0.36f;
            p[i] = c + ImVec2(std::cos(a) * r, std::sin(a) * r + h * 0.06f);
        }
        if (icon == Icon::StarFilled) dl->AddConcavePolyFilled(p, 10, color);
        else dl->AddPolyline(p, 10, color, t * 1.1f, ImDrawFlags_Closed);
        break;
    }
    case Icon::More:
        for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(c + ImVec2(static_cast<float>(i) * h * 0.5f, 0), t * 1.3f, color);
        break;
    case Icon::Info:
        dl->AddCircle(c, h * 0.72f, color, 20, t * 1.3f);
        dl->AddLine(c + ImVec2(0, -h * 0.05f), c + ImVec2(0, h * 0.4f), color, t * 1.6f);
        dl->AddCircleFilled(c + ImVec2(0, -h * 0.33f), t * 1.0f, color);
        break;
    case Icon::Bolt: {
        const ImVec2 p[6] = {c + ImVec2(h * 0.15f, -h * 0.85f), c + ImVec2(-h * 0.55f, h * 0.1f), c + ImVec2(-h * 0.05f, h * 0.1f),
                             c + ImVec2(-h * 0.2f, h * 0.85f), c + ImVec2(h * 0.55f, -h * 0.15f), c + ImVec2(h * 0.05f, -h * 0.15f)};
        dl->AddConcavePolyFilled(p, 6, color);
        break;
    }
    case Icon::Volume:
        drawIcon(dl, Icon::Speaker, c, s, color);
        break;
    }
}

bool iconButton(const char* id, Icon icon, const char* tip, bool active, float size) {
    // Ghost style: no background until hovered; active = accent tint.
    const float s = size > 0 ? size : ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::min(pal::kControlRadius, s * 0.3f);
    if (active) dl->AddRectFilled(pos, pos + ImVec2(s, s), pal::kAccentSoft, r);
    else if (held) dl->AddRectFilled(pos, pos + ImVec2(s, s), pal::kPanelActive, r);
    else if (hovered && !disabled) dl->AddRectFilled(pos, pos + ImVec2(s, s), pal::kPanelHover, r);
    const ImU32 fg = disabled ? pal::kTextFaint : active ? pal::kAccentText : hovered ? pal::kText : pal::withAlpha(pal::kText, 0.82f);
    drawIcon(dl, icon, pos + ImVec2(s * 0.5f, s * 0.5f), s * 0.56f, fg);
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
    // Secondary button: soft filled pill with an icon.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float iconW = h * 0.85f;
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size(iconW + textSize.x + st.FramePadding.x * 2.0f + 2.0f, h);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = active ? pal::kAccentSoft : held ? pal::kPanelActive : (hovered && enabled) ? pal::kPanelHover : pal::kPanelRaised;
    dl->AddRectFilled(pos, pos + size, bg, pal::kControlRadius);
    const ImU32 fg = !enabled ? pal::kTextFaint : active ? pal::kAccentText : pal::kText;
    drawIcon(dl, icon, pos + ImVec2(st.FramePadding.x + iconW * 0.42f, h * 0.5f), h * 0.5f, fg);
    dl->AddText(pos + ImVec2(st.FramePadding.x + iconW, (h - textSize.y) * 0.5f), fg, label, ImGui::FindRenderedTextEnd(label));
    ImGui::EndDisabled();
    if (tip) tooltip(tip);
    return pressed && enabled;
}

bool primaryButton(const char* label, ImVec2 size, bool enabled) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    if (size.x <= 0) size.x = ts.x + st.FramePadding.x * 2.6f;
    if (size.y <= 0) size.y = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 top = !enabled ? pal::kPanelHover : held ? pal::kAccentPressed : hovered ? pal::kAccentHover : pal::kAccent;
    const ImU32 bottom = !enabled ? pal::kPanelHover : scaleColor(top, 0.86f);
    // Subtle vertical gradient reads as a modern filled button.
    dl->AddRectFilled(pos, pos + size, bottom, pal::kControlRadius);
    dl->AddRectFilledMultiColor(pos + ImVec2(pal::kControlRadius * 0.5f, 1), pos + ImVec2(size.x - pal::kControlRadius * 0.5f, size.y * 0.5f), top, top,
                                bottom, bottom);
    const ImU32 fg = enabled ? IM_COL32(255, 255, 255, 255) : pal::kTextFaint;
    dl->AddText(pos + ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f), fg, label, ImGui::FindRenderedTextEnd(label));
    return pressed && enabled;
}

bool ghostButton(const char* id, Icon icon, const char* label, const char* tip, bool active, bool enabled) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float iconW = h * 0.8f;
    const bool hasLabel = label && label[0] && label[0] != '#';
    const ImVec2 textSize = hasLabel ? ImGui::CalcTextSize(label, nullptr, true) : ImVec2(0, 0);
    const ImVec2 size(hasLabel ? iconW + textSize.x + st.FramePadding.x * 1.6f : h, h);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(pos, pos + size, pal::kAccentSoft, pal::kControlRadius);
    else if (held) dl->AddRectFilled(pos, pos + size, pal::kPanelActive, pal::kControlRadius);
    else if (hovered && enabled) dl->AddRectFilled(pos, pos + size, pal::kPanelHover, pal::kControlRadius);
    const ImU32 fg = !enabled ? pal::kTextFaint : active ? pal::kAccentText : pal::kText;
    const float iconX = hasLabel ? st.FramePadding.x * 0.6f + iconW * 0.45f : h * 0.5f;
    drawIcon(dl, icon, pos + ImVec2(iconX, h * 0.5f), h * 0.5f, fg);
    if (hasLabel)
        dl->AddText(pos + ImVec2(st.FramePadding.x * 0.6f + iconW, (h - textSize.y) * 0.5f), fg, label, ImGui::FindRenderedTextEnd(label));
    if (tip) tooltip(tip);
    return pressed && enabled;
}

bool segmented(const char* id, const char* const* labels, int count, int* current, const char* const* tips) {
    ImGui::PushID(id);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    float total = 0;
    std::vector<float> widths(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        widths[static_cast<size_t>(i)] = ImGui::CalcTextSize(labels[i], nullptr, true).x + st.FramePadding.x * 2.2f;
        total += widths[static_cast<size_t>(i)];
    }
    const float pad = 3.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + ImVec2(total + pad * 2, h), pal::kPanelRaised, h * 0.5f);
    bool changed = false;
    float x = pos.x + pad;
    for (int i = 0; i < count; ++i) {
        const float w = widths[static_cast<size_t>(i)];
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y + pad));
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##seg", ImVec2(w, h - pad * 2));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (tips && tips[i]) tooltip(tips[i]);
        const bool sel = *current == i;
        if (sel) dl->AddRectFilled(ImVec2(x, pos.y + pad), ImVec2(x + w, pos.y + h - pad), pal::kAccent, (h - pad * 2) * 0.5f);
        else if (hovered) dl->AddRectFilled(ImVec2(x, pos.y + pad), ImVec2(x + w, pos.y + h - pad), pal::kPanelHover, (h - pad * 2) * 0.5f);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i], nullptr, true);
        dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), sel ? IM_COL32(255, 255, 255, 255) : hovered ? pal::kText : pal::kTextMuted,
                    labels[i], ImGui::FindRenderedTextEnd(labels[i]));
        if (pressed && !sel) {
            *current = i;
            changed = true;
        }
        x += w;
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(total + pad * 2, h));
    ImGui::PopID();
    return changed;
}

bool railButton(const char* id, Icon icon, const char* label, bool active, float width) {
    const float fs = ImGui::GetFontSize();
    const float h = fs * 3.4f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = pos + ImVec2(4, 2), b = pos + ImVec2(width - 4, h - 2);
    if (active) {
        dl->AddRectFilled(a, b, pal::kAccentSoft, pal::kControlRadius + 2);
        dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.28f), ImVec2(pos.x + 3, pos.y + h * 0.72f), pal::kAccent, 2.0f);
    } else if (hovered) {
        dl->AddRectFilled(a, b, pal::kPanelHover, pal::kControlRadius + 2);
    }
    const ImU32 fg = active ? pal::kAccentText : hovered ? pal::kText : pal::kTextMuted;
    drawIcon(dl, icon, ImVec2(pos.x + width * 0.5f, pos.y + h * 0.36f), fs * 1.15f, fg);
    // Labels use a smaller size so long Japanese words fit.
    float small = fs * 0.74f;
    ImFont* font = ImGui::GetFont();
    ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, label);
    if (ts.x > width - 6) {
        small *= (width - 6) / ts.x;
        ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, label);
    }
    dl->AddText(font, small, ImVec2(pos.x + (width - ts.x) * 0.5f, pos.y + h * 0.66f), fg, label);
    if (hovered) tooltip(label);
    return pressed;
}

bool chipButton(const char* label, bool active) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight() * 0.86f;
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size(ts.x + st.FramePadding.x * 1.8f, h);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(pos, pos + size, pal::kAccent, h * 0.5f);
    else {
        dl->AddRectFilled(pos, pos + size, hovered ? pal::kPanelHover : pal::kPanelRaised, h * 0.5f);
        dl->AddRect(pos, pos + size, pal::kStroke, h * 0.5f);
    }
    dl->AddText(pos + ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f), active ? IM_COL32(255, 255, 255, 255) : hovered ? pal::kText : pal::kTextMuted,
                label, ImGui::FindRenderedTextEnd(label));
    return pressed;
}

bool checkbox(const char* label, bool* v) {
    ImGui::PushID(label);
    const float fs = ImGui::GetFontSize();
    const float box = std::round(fs * 1.0f);
    const float h = ImGui::GetFrameHeight();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x + 2.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##cb", ImVec2(box + (ts.x > 0 ? gap + ts.x : 0.0f), h));
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(ImGui::GetItemID());
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y + std::round((h - box) * 0.5f)), b = a + ImVec2(box, box);
    if (*v) {
        dl->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? pal::kAccentHover : pal::kAccent), 4.0f);
        dl->PathLineTo(a + ImVec2(box * 0.25f, box * 0.52f));
        dl->PathLineTo(a + ImVec2(box * 0.43f, box * 0.70f));
        dl->PathLineTo(a + ImVec2(box * 0.76f, box * 0.32f));
        dl->PathStroke(ImGui::GetColorU32(IM_COL32(255, 255, 255, 255)), std::max(1.6f, box * 0.13f));
    } else {
        dl->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? pal::kPanelHover : pal::kPanelRaised), 4.0f);
        dl->AddRect(a, b, ImGui::GetColorU32(IM_COL32(255, 255, 255, hovered ? 120 : 64)), 4.0f, 1.4f);
    }
    if (ts.x > 0) dl->AddText(ImVec2(b.x + gap, pos.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label, end);
    ImGui::PopID();
    return pressed;
}

bool toggleSwitch(const char* id, bool* v, const char* tip) {
    const float fs = ImGui::GetFontSize();
    const float th = std::round(fs * 1.05f), tw = std::round(th * 1.8f);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(tw, h));
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(ImGui::GetItemID());
    }
    if (tip) tooltip(tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y + std::round((h - th) * 0.5f)), b = a + ImVec2(tw, th);
    const ImU32 track = *v ? (hovered ? pal::kAccentHover : pal::kAccent) : IM_COL32(255, 255, 255, hovered ? 60 : 38);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(track), th * 0.5f);
    const float r = th * 0.5f - 2.5f;
    const ImVec2 knob(*v ? b.x - th * 0.5f : a.x + th * 0.5f, a.y + th * 0.5f);
    dl->AddCircleFilled(knob + ImVec2(0, 1), r, ImGui::GetColorU32(IM_COL32(0, 0, 0, 60)));
    dl->AddCircleFilled(knob, r, ImGui::GetColorU32(IM_COL32(255, 255, 255, 255)));
    return pressed;
}

bool underlineTabs(const char* id, const char* const* labels, int count, int* current) {
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() + 2.0f;
    ImFont* bold = boldFont() ? boldFont() : ImGui::GetFont();
    float size = ImGui::GetFontSize();
    float gap = size * 1.25f;
    auto measure = [&] {
        float total = 0;
        for (int i = 0; i < count; ++i) total += bold->CalcTextSizeA(size, FLT_MAX, 0.0f, labels[i], ImGui::FindRenderedTextEnd(labels[i])).x;
        return total + gap * static_cast<float>(count - 1);
    };
    // Squeeze the gaps first, then the text, so all tabs stay visible.
    if (float total = measure(); total > avail) {
        gap = std::max(size * 0.6f, gap - (total - avail) / std::max(1, count - 1));
        if (total = measure(); total > avail) size = std::max(ImGui::GetFontSize() * 0.75f, size * avail / total);
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(pos.x, pos.y + h - 0.5f), ImVec2(pos.x + avail, pos.y + h - 0.5f), pal::kStroke);
    bool changed = false;
    float x = pos.x;
    for (int i = 0; i < count; ++i) {
        const char* end = ImGui::FindRenderedTextEnd(labels[i]);
        const ImVec2 ts = bold->CalcTextSizeA(size, FLT_MAX, 0.0f, labels[i], end);
        ImGui::SetCursorScreenPos(ImVec2(x - gap * 0.5f + (i == 0 ? gap * 0.5f : 0.0f), pos.y));
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##tab", ImVec2(ts.x + (i == 0 ? gap * 0.5f : gap), h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = *current == i;
        dl->AddText(sel ? bold : ImGui::GetFont(), size, ImVec2(x, pos.y + (h - ts.y) * 0.5f - 1),
                    sel ? pal::kText : hovered ? pal::kText : pal::kTextMuted, labels[i], end);
        if (sel) dl->AddRectFilled(ImVec2(x, pos.y + h - 2.5f), ImVec2(x + ts.x, pos.y + h), pal::kAccent, 1.5f);
        if (pressed && !sel) {
            *current = i;
            changed = true;
        }
        x += ts.x + gap;
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(avail, h));
    ImGui::PopID();
    return changed;
}

namespace {
struct TabsState {
    ImGuiID id = 0;
    ImVec2 pos;
    std::vector<std::string> labels;
    int selected = 0;
};
std::vector<TabsState> g_tabs;  // nesting stack (UI thread only)
}  // namespace

bool beginTabs(const char* id) {
    TabsState st;
    st.id = ImGui::GetID(id);
    st.pos = ImGui::GetCursorScreenPos();
    st.selected = ImGui::GetStateStorage()->GetInt(st.id, 0);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + 2.0f));
    ImGui::Spacing();
    g_tabs.push_back(std::move(st));
    return true;
}

bool tabItem(const char* label) {
    if (g_tabs.empty()) return false;
    TabsState& st = g_tabs.back();
    st.labels.emplace_back(label);
    return static_cast<int>(st.labels.size()) - 1 == st.selected;
}

void endTabs() {
    if (g_tabs.empty()) return;
    TabsState st = std::move(g_tabs.back());
    g_tabs.pop_back();
    const int n = static_cast<int>(st.labels.size());
    if (n == 0) return;
    int sel = std::clamp(st.selected, 0, n - 1);
    std::vector<const char*> ptrs;
    for (const auto& l : st.labels) ptrs.push_back(l.c_str());
    const ImVec2 end = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(st.pos);
    ImGui::PushID(static_cast<int>(st.id));
    underlineTabs("##bar", ptrs.data(), n, &sel);
    ImGui::PopID();
    if (sel != st.selected) ImGui::GetStateStorage()->SetInt(st.id, sel);
    ImGui::SetCursorScreenPos(end);
    ImGui::Dummy(ImVec2(0, 0));
}

bool iconTab(const char* id, Icon icon, const char* label, bool active, ImVec2 size) {
    const float fs = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered && !active) dl->AddRectFilled(pos + ImVec2(2, 3), pos + size - ImVec2(2, 5), pal::kPanelHover, pal::kControlRadius);
    const ImU32 fg = active ? pal::kAccentText : hovered ? pal::kText : pal::kTextMuted;
    drawIcon(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.36f), fs * 1.12f, fg);
    float small = fs * 0.76f;
    ImFont* font = active && boldFont() ? boldFont() : ImGui::GetFont();
    ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, label);
    if (ts.x > size.x - 4) {
        small *= (size.x - 4) / ts.x;
        ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, label);
    }
    dl->AddText(font, small, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + size.y * 0.64f), fg, label);
    if (active) {
        const float iw = std::min(size.x - 12, fs * 1.6f);
        dl->AddRectFilled(ImVec2(pos.x + (size.x - iw) * 0.5f, pos.y + size.y - 3), ImVec2(pos.x + (size.x + iw) * 0.5f, pos.y + size.y),
                          pal::kAccent, 1.5f);
    }
    if (hovered) tooltip(label);
    return pressed;
}

void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, ImDrawFlags flags, bool diagonal) {
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 255), rounding, flags);
    const ImVec2 p1 = diagonal ? b : ImVec2(b.x, a.y);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, p1, c0, c1);
    // Keep the gradient's own alpha (ShadeVerts keeps the white's alpha).
    const ImU32 alpha = std::min(c0 >> 24, c1 >> 24);
    if (alpha < 255)
        for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
            ImU32& col = dl->VtxBuffer[i].col;
            col = (col & 0x00FFFFFFu) | ((((col >> 24) * alpha) / 255u) << 24);
        }
}

void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float rounding, float thickness, float dash, float gap) {
    dl->PathRect(a, b, rounding);
    const ImVector<ImVec2> pts = dl->_Path;
    dl->PathClear();
    if (pts.Size < 2) return;
    float phase = 0.0f;  // distance travelled inside the current dash+gap period
    const float period = dash + gap;
    for (int i = 0; i < pts.Size; ++i) {
        const ImVec2 p = pts[i], q = pts[(i + 1) % pts.Size];
        const ImVec2 d = q - p;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        float t = 0.0f;
        while (t < len) {
            const float inPeriod = std::fmod(phase, period);
            const float step = std::min(len - t, inPeriod < dash ? dash - inPeriod : period - inPeriod);
            if (inPeriod < dash) dl->AddLine(p + d * (t / len), p + d * ((t + step) / len), col, thickness);
            t += step;
            phase += step;
        }
    }
}

bool dropZone(const char* id, ImVec2 size, Icon icon, const char* title, const char* subtitle, const char* note) {
    const float fs = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool dropping = ImGui::GetDragDropPayload() != nullptr || hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = pal::kControlRadius + 4;
    dl->AddRectFilled(pos, pos + size, hovered ? pal::withAlpha(pal::kAccent, 0.08f) : IM_COL32(255, 255, 255, 6), r);
    dashedRect(dl, pos + ImVec2(1, 1), pos + size - ImVec2(1, 1), dropping ? pal::withAlpha(pal::kAccent, 0.85f) : IM_COL32(255, 255, 255, 52), r);
    // Content block centred vertically: icon disc, title, subtitle, note.
    const float wrap = size.x - fs * 2;
    ImFont* bold = boldFont() ? boldFont() : ImGui::GetFont();
    auto linesH = [&](ImFont* f, float sz, const char* t) {
        return t ? static_cast<float>(wrapLines(f, sz, t, nullptr, wrap).size()) * sz * 1.25f : 0.0f;
    };
    const ImVec2 ts(0, linesH(bold, fs * 1.05f, title));
    const ImVec2 ss(0, linesH(ImGui::GetFont(), fs * 0.92f, subtitle));
    const ImVec2 ns(0, linesH(ImGui::GetFont(), fs * 0.8f, note));
    const float disc = fs * 3.0f;
    const float blockH = disc + 12 + ts.y + (subtitle ? 2 + ss.y : 0) + (note ? 8 + ns.y : 0);
    float y = pos.y + std::max(8.0f, (size.y - blockH) * 0.5f);
    const float cx = pos.x + size.x * 0.5f;
    gradientRect(dl, ImVec2(cx - disc * 0.5f, y), ImVec2(cx + disc * 0.5f, y + disc), pal::kAccentHover, pal::mix(pal::kAccent, pal::kAccent2, 0.6f),
                 disc * 0.5f);
    drawIcon(dl, icon, ImVec2(cx, y + disc * 0.5f), fs * 1.3f, IM_COL32(255, 255, 255, 255));
    y += disc + 12;
    dl->PushClipRect(pos, pos + size, true);
    auto centered = [&](ImFont* f, float sz, ImU32 col, const char* text, ImVec2 tsz) {
        for (const auto& [b, e] : wrapLines(f, sz, text, nullptr, wrap)) {
            const ImVec2 lsz = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, b, e);
            dl->AddText(f, sz, ImVec2(cx - lsz.x * 0.5f, y), col, b, e);
            y += sz * 1.25f;
        }
        (void)tsz;
    };
    centered(bold, fs * 1.05f, pal::kText, title, ts);
    if (subtitle) {
        y += 2;
        centered(ImGui::GetFont(), fs * 0.92f, pal::kTextMuted, subtitle, ss);
    }
    if (note) {
        y += 8;
        centered(ImGui::GetFont(), fs * 0.8f, pal::kTextFaint, note, ns);
    }
    dl->PopClipRect();
    return pressed;
}

bool navItem(const char* id, Icon icon, const char* label, bool active) {
    const float fs = ImGui::GetFontSize();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = fs * 2.1f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) {
        dl->AddRectFilled(pos, pos + ImVec2(w, h), pal::kPanelActive, pal::kControlRadius);
        dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.28f), ImVec2(pos.x + 3, pos.y + h * 0.72f), pal::kAccent, 1.5f);
    } else if (hovered) {
        dl->AddRectFilled(pos, pos + ImVec2(w, h), pal::kPanelHover, pal::kControlRadius);
    }
    const ImU32 fg = active ? pal::kText : hovered ? pal::kText : pal::kTextMuted;
    drawIcon(dl, icon, ImVec2(pos.x + fs * 1.2f, pos.y + h * 0.5f), fs * 0.95f, active ? pal::kAccentText : fg);
    dl->PushClipRect(pos, pos + ImVec2(w - 4, h), true);
    dl->AddText(ImVec2(pos.x + fs * 2.3f, pos.y + (h - fs) * 0.5f), fg, label);
    dl->PopClipRect();
    if (hovered) tooltip(label);
    return pressed;
}

void floatingWindowDecor() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->DockIsActive || (w->Flags & ImGuiWindowFlags_ChildWindow)) return;
    ImDrawList* dl = w->DrawList;
    const float r = ImGui::GetStyle().WindowRounding;
    const ImVec2 a = w->Pos, b = w->Pos + w->Size;
    dl->PushClipRectFullScreen();
    for (int i = 1; i <= 14; ++i) {
        const float k = static_cast<float>(i);
        const float t = 1.0f - k / 15.0f;
        dl->AddRect(a - ImVec2(k, k - 2), b + ImVec2(k, k + 2), IM_COL32(0, 0, 0, static_cast<int>(70 * t * t)), r + k, 1.6f);
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 26), r, 1.0f);
    dl->PopClipRect();
}

const AspectPreset* aspectPresets(int* count) {
    static constexpr AspectPreset kPresets[] = {
        {"16:9", 16, 9, trNoop("Landscape (YouTube, TV)")}, {"9:16", 9, 16, trNoop("Vertical (Shorts, TikTok, Reels)")},
        {"1:1", 1, 1, trNoop("Square")},                    {"4:5", 4, 5, trNoop("Portrait (Instagram feed)")},
        {"21:9", 21, 9, trNoop("Cinema widescreen")},
    };
    if (count) *count = static_cast<int>(std::size(kPresets));
    return kPresets;
}

bool accentSwatches(const char* id, std::string& accent) {
    ImGui::PushID(id);
    int count = 0;
    const pal::AccentPreset* presets = pal::accentPresets(&count);
    const float d = ImGui::GetFrameHeight();
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0, 8);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##sw", ImVec2(d, d));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        tooltip(tr(presets[i].label));
        const bool sel = accent == presets[i].id;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = pos + ImVec2(d * 0.5f, d * 0.5f);
        if (sel || hovered) dl->AddCircle(c, d * 0.5f - 0.5f, sel ? pal::kText : IM_COL32(255, 255, 255, 90), 0, 2.0f);
        dl->AddCircleFilled(c, d * 0.5f - 4.0f, presets[i].swatch);
        if (sel) {
            const float s = d * 0.16f;
            dl->PathLineTo(c + ImVec2(-s, 0));
            dl->PathLineTo(c + ImVec2(-s * 0.3f, s * 0.75f));
            dl->PathLineTo(c + ImVec2(s * 1.05f, -s * 0.7f));
            dl->PathStroke(IM_COL32(255, 255, 255, 255), 2.0f);
        }
        if (pressed && !sel) {
            accent = presets[i].id;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

void headingText(const char* text, float scale) {
    ImFont* bold = boldFont();
    ImGui::PushFont(bold ? bold : ImGui::GetFont(), ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void beginCard(const char* id, ImVec2 size, ImU32 bg) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, pal::kControlRadius + 2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding | (size.y <= 0 ? ImGuiChildFlags_AutoResizeY : 0));
}

void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool vFader(const char* id, float* value, float minV, float maxV, ImVec2 size) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    const float knobH = std::min(18.0f, size.y * 0.12f);
    const float top = pos.y + knobH * 0.5f, bottom = pos.y + size.y - knobH * 0.5f;
    if (active) {
        const float t = std::clamp((bottom - ImGui::GetIO().MousePos.y) / std::max(1.0f, bottom - top), 0.0f, 1.0f);
        const float v = minV + t * (maxV - minV);
        if (v != *value) {
            *value = v;
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = pos.x + size.x * 0.5f;
    const float t = std::clamp((*value - minV) / (maxV - minV), 0.0f, 1.0f);
    const float ky = bottom - t * (bottom - top);
    dl->AddRectFilled(ImVec2(cx - 2, top), ImVec2(cx + 2, bottom), pal::kPanelHover, 2.0f);
    dl->AddRectFilled(ImVec2(cx - 2, ky), ImVec2(cx + 2, bottom), active || hovered ? pal::kAccentHover : pal::kAccent, 2.0f);
    // Unity (0) tick when the range covers it.
    if (minV < 0 && maxV > 0) {
        const float uy = bottom - (-minV / (maxV - minV)) * (bottom - top);
        dl->AddLine(ImVec2(pos.x + 2, uy), ImVec2(pos.x + size.x - 2, uy), pal::kStrokeStrong);
    }
    const float kw = size.x * 0.5f;
    dl->AddRectFilled(ImVec2(cx - kw, ky - knobH * 0.5f), ImVec2(cx + kw, ky + knobH * 0.5f), active ? IM_COL32(255, 255, 255, 255) : IM_COL32(222, 224, 232, 255),
                      4.0f);
    dl->AddLine(ImVec2(cx - kw + 4, ky), ImVec2(cx + kw - 4, ky), IM_COL32(40, 40, 50, 200), 1.5f);
    return changed;
}

bool slimSlider(const char* id, float* value, float minV, float maxV, float width, bool centered) {
    const float h = ImGui::GetFrameHeight() * 0.7f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const float r = 6.0f;
    const float x0 = pos.x + r, x1 = pos.x + width - r;
    bool changed = false;
    if (active) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        const float v = minV + t * (maxV - minV);
        if (v != *value) {
            *value = v;
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = pos.y + h * 0.5f;
    const float t = std::clamp((*value - minV) / (maxV - minV), 0.0f, 1.0f);
    const float kx = x0 + t * (x1 - x0);
    dl->AddRectFilled(ImVec2(x0, cy - 2), ImVec2(x1, cy + 2), pal::kPanelHover, 2.0f);
    const float from = centered ? (x0 + x1) * 0.5f : x0;
    dl->AddRectFilled(ImVec2(std::min(from, kx), cy - 2), ImVec2(std::max(from, kx), cy + 2), pal::kAccent, 2.0f);
    if (centered) dl->AddLine(ImVec2((x0 + x1) * 0.5f, cy - 5), ImVec2((x0 + x1) * 0.5f, cy + 5), pal::kStrokeStrong);
    dl->AddCircleFilled(ImVec2(kx, cy), active || hovered ? r + 1 : r, IM_COL32(240, 241, 247, 255), 20);
    return changed;
}

void statusDot(ImU32 color) {
    const float r = ImGui::GetFontSize() * 0.22f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, color);
    ImGui::Dummy(ImVec2(r * 2, h));
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

namespace {
// Characters that must not start a line (closing brackets, Japanese
// punctuation, small kana) and ones that must not end one (opening brackets).
bool noLineStart(unsigned c) {
    switch (c) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0x300D: case 0x300F: case 0xFF09: case 0x3011:
    case 0xFF01: case 0xFF1F: case 0x30FC: case 0x3005: case 0x30FB: case 0xFF1A: case 0xFF1B:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085: case 0x3087:
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7:
    case ',': case '.': case ')': case ']': case '!': case '?': case ':': case ';':
        return true;
    default: return false;
    }
}
bool noLineEnd(unsigned c) { return c == 0x300C || c == 0x300E || c == 0xFF08 || c == 0x3010 || c == '(' || c == '['; }
bool isWordChar(unsigned c) { return c < 0x80 && (std::isalnum(static_cast<int>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == '%' || c == '+'); }

struct Cp {
    const char* p;
    unsigned c;
};
}  // namespace

std::vector<std::pair<const char*, const char*>> wrapLines(ImFont* font, float size, const char* text, const char* end, float width) {
    std::vector<std::pair<const char*, const char*>> lines;
    if (!text) return lines;
    if (!end) end = text + std::strlen(text);
    const char* s = text;
    while (s < end) {
        // Longest prefix that fits (up to a newline).
        std::vector<Cp> cps;
        const char* p = s;
        float w = 0.0f;
        bool newline = false;
        while (p < end) {
            unsigned c = 0;
            const int n = ImTextCharFromUtf8(&c, p, end);
            if (c == '\n') {
                newline = true;
                break;
            }
            const float cw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + n).x;
            if (w + cw > width && !cps.empty()) break;
            cps.push_back({p, c});
            w += cw;
            p += n;
        }
        if (p >= end || newline) {
            lines.emplace_back(s, p);
            s = newline ? p + 1 : p;
            continue;
        }
        // Must break before cps[k] (k = index of the first char of the next line).
        size_t k = cps.size();
        unsigned next = 0;
        ImTextCharFromUtf8(&next, p, end);
        const size_t minKeep = std::max<size_t>(1, cps.size() * 2 / 5);
        if (isWordChar(next) && isWordChar(cps.back().c)) {
            // Mid Latin word: break before the word if it starts on this line.
            size_t j = k;
            while (j > minKeep && isWordChar(cps[j - 1].c)) --j;
            if (j > minKeep) k = j;
        }
        // Kinsoku: pull a forbidden line-start character back onto this line
        // by breaking one character earlier, and never end on an opening bracket.
        if (k == cps.size() && noLineStart(next) && k > minKeep) --k;
        while (k > minKeep && k < cps.size() && noLineStart(cps[k].c)) --k;
        while (k > minKeep && noLineEnd(cps[k - 1].c)) --k;
        const char* brk = k < cps.size() ? cps[k].p : p;
        const char* lineEnd = brk;
        while (lineEnd > s && lineEnd[-1] == ' ') --lineEnd;
        lines.emplace_back(s, lineEnd);
        s = brk;
        while (s < end && *s == ' ') ++s;
    }
    return lines;
}

void wrappedText(const char* text) {
    // Same width rules as ImGui::TextWrapped: an explicit wrap position wins;
    // auto-resizing windows (modals, tooltips) get a comfortable fixed width.
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    float width = ImGui::GetContentRegionAvail().x;
    if (win->DC.TextWrapPos > 0.0f) width = win->DC.TextWrapPos - ImGui::GetCursorPosX();
    else if (win->Flags & (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Tooltip)) width = ImGui::GetFontSize() * 30.0f;
    width = std::max(ImGui::GetFontSize() * 4, width);
    for (const auto& [b, e] : wrapLines(ImGui::GetFont(), ImGui::GetFontSize(), text, nullptr, width)) ImGui::TextUnformatted(b, e);
}

void hintText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    wrappedText(text);
    ImGui::PopStyleColor();
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
    auto norm = [](float lin) { return std::clamp((linearToDb(lin) + 60.0f) / 66.0f, 0.0f, 1.0f); };
    // Zones: green up to -18 dB, amber to -6 dB, red above.
    const float zAmber = (60.0f - 18.0f) / 66.0f, zRed = (60.0f - 6.0f) / 66.0f;
    const ImU32 green = IM_COL32(52, 211, 120, 255), amber = IM_COL32(250, 190, 50, 255), red = IM_COL32(255, 86, 92, 255);
    for (int ch = 0; ch < 2; ++ch) {
        const float pk = norm(m.peak[ch]), rms = norm(m.rms[ch]);
        if (vertical) {
            const float w = std::max(2.0f, (size.x - 2) * 0.5f);
            const float x0 = pos.x + static_cast<float>(ch) * (w + 2);
            auto y = [&](float n) { return pos.y + size.y - n * size.y; };
            dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x0 + w, pos.y + size.y), IM_COL32(8, 9, 12, 255), 2.0f);
            auto seg = [&](float a, float b, ImU32 c) {
                const float top = std::min(rms, b);
                if (top > a) dl->AddRectFilled(ImVec2(x0, y(top)), ImVec2(x0 + w, y(a)), c, 1.5f);
            };
            seg(0.0f, zAmber, green);
            seg(zAmber, zRed, amber);
            seg(zRed, 1.0f, red);
            const float yp = y(pk);
            dl->AddLine(ImVec2(x0, yp), ImVec2(x0 + w, yp), pk > zRed ? red : pk > zAmber ? amber : IM_COL32(170, 240, 190, 255), 2.0f);
        } else {
            const float hgt = (size.y - 2) * 0.5f;
            const float y0 = pos.y + static_cast<float>(ch) * (hgt + 2);
            dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(pos.x + size.x, y0 + hgt), IM_COL32(8, 9, 12, 255), 2.0f);
            dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(pos.x + rms * size.x, y0 + hgt), rms > zRed ? red : rms > zAmber ? amber : green, 2.0f);
            const float xp = pos.x + pk * size.x;
            dl->AddLine(ImVec2(xp, y0), ImVec2(xp, y0 + hgt), IM_COL32(240, 240, 240, 220), 2.0f);
        }
    }
    if (m.clipped) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + 4), red, 2.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("L %.1f dB  R %.1f dB", linearToDb(m.peak[0]), linearToDb(m.peak[1]));
    (void)id;
}

// ------------------------------------------------------------------ parameters

bool sectionHeader(const char* label, bool defaultOpen) {
    // Flat header: bold title and a chevron, separated by a hairline (the
    // open state lives in the window storage, like a tree node).
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(label);
    bool open = storage->GetBool(key, defaultOpen);
    const float fs = ImGui::GetFontSize();
    const float h = ImGui::GetFrameHeight() + 4.0f;
    ImGui::Dummy(ImVec2(0, 1));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##section", ImVec2(w, h));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) {
        open = !open;
        storage->SetBool(key, open);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x + w, pos.y), pal::kStroke);
    if (hovered) dl->AddRectFilled(pos + ImVec2(-4, 2), pos + ImVec2(w + 4, h - 1), IM_COL32(255, 255, 255, 8), pal::kControlRadius);
    ImFont* bold = boldFont() ? boldFont() : ImGui::GetFont();
    const char* end = ImGui::FindRenderedTextEnd(label);
    dl->AddText(bold, fs * 0.98f, ImVec2(pos.x + 2, pos.y + (h - fs) * 0.5f + 1), open || hovered ? pal::kText : pal::kTextMuted, label, end);
    const ImVec2 c(pos.x + w - fs * 0.55f, pos.y + h * 0.5f + 1);
    const float s = fs * 0.24f;
    const ImU32 cc = hovered ? pal::kText : pal::kTextMuted;
    if (open) {
        dl->PathLineTo(c + ImVec2(-s, -s * 0.5f));
        dl->PathLineTo(c + ImVec2(0, s * 0.5f));
        dl->PathLineTo(c + ImVec2(s, -s * 0.5f));
    } else {
        dl->PathLineTo(c + ImVec2(-s * 0.5f, -s));
        dl->PathLineTo(c + ImVec2(s * 0.5f, 0));
        dl->PathLineTo(c + ImVec2(-s * 0.5f, s));
    }
    dl->PathStroke(cc, 1.6f);
    return open;
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
        edited = checkbox("##v", &b);
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
