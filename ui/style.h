#pragma once
// Design tokens of the editor UI: one palette and a few spacing/radius values
// shared by the theme, the panels and the custom-drawn widgets, so the whole
// app reads as one modern, consistent surface.

#include <imgui.h>

namespace avc::ui::pal {

// Surfaces (dark, neutral with a hint of blue).
inline constexpr ImU32 kAppBg = IM_COL32(11, 12, 15, 255);      // behind the panels (the gaps)
inline constexpr ImU32 kPanel = IM_COL32(22, 23, 28, 255);      // panel ("card") surface
inline constexpr ImU32 kPanelRaised = IM_COL32(30, 32, 38, 255);  // inputs, track headers, tiles
inline constexpr ImU32 kPanelHover = IM_COL32(39, 42, 50, 255);
inline constexpr ImU32 kPanelActive = IM_COL32(47, 51, 61, 255);
inline constexpr ImU32 kStroke = IM_COL32(255, 255, 255, 16);   // hairlines
inline constexpr ImU32 kStrokeStrong = IM_COL32(255, 255, 255, 34);

// Text.
inline constexpr ImU32 kText = IM_COL32(236, 238, 244, 255);
inline constexpr ImU32 kTextMuted = IM_COL32(152, 157, 172, 255);
inline constexpr ImU32 kTextFaint = IM_COL32(104, 109, 124, 255);

// Accent (violet) and states.
inline constexpr ImU32 kAccent = IM_COL32(124, 92, 255, 255);
inline constexpr ImU32 kAccentHover = IM_COL32(145, 118, 255, 255);
inline constexpr ImU32 kAccentPressed = IM_COL32(104, 74, 230, 255);
inline constexpr ImU32 kAccentSoft = IM_COL32(124, 92, 255, 46);
inline constexpr ImU32 kAccentText = IM_COL32(178, 160, 255, 255);
inline constexpr ImU32 kSuccess = IM_COL32(48, 196, 125, 255);
inline constexpr ImU32 kWarning = IM_COL32(255, 178, 46, 255);
inline constexpr ImU32 kDanger = IM_COL32(255, 86, 92, 255);
inline constexpr ImU32 kPlayhead = IM_COL32(255, 92, 116, 255);

// Clip colours (saturated but not neon).
inline constexpr ImU32 kClipVideo = IM_COL32(56, 116, 236, 255);
inline constexpr ImU32 kClipAudio = IM_COL32(28, 170, 128, 255);
inline constexpr ImU32 kClipText = IM_COL32(150, 92, 240, 255);
inline constexpr ImU32 kClipSubtitle = IM_COL32(226, 82, 148, 255);
inline constexpr ImU32 kClipAdjust = IM_COL32(236, 128, 40, 255);
inline constexpr ImU32 kClipSolid = IM_COL32(104, 110, 128, 255);
inline constexpr ImU32 kClipImage = IM_COL32(84, 96, 220, 255);
inline constexpr ImU32 kClipCompound = IM_COL32(32, 150, 176, 255);

// Geometry.
inline constexpr float kPanelRadius = 10.0f;
inline constexpr float kControlRadius = 7.0f;
inline constexpr float kPanelGap = 6.0f;

inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
inline ImU32 withAlpha(ImU32 c, float a) {
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(static_cast<float>((c >> 24) & 0xFF) * a) << 24);
}

}  // namespace avc::ui::pal
