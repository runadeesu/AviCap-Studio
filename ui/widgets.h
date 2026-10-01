#pragma once
// Shared drawing helpers: colours, vector icons (independent of font
// coverage), timecode fields, level meters and the keyframable parameter
// editor used by the inspector, effects and colour panels.

#include <optional>
#include <string>

#include <imgui.h>

#include "audio/mixer.h"
#include "timeline/model.h"

namespace avc::ui {

namespace col {
inline constexpr ImU32 kVideo = IM_COL32(61, 111, 182, 255);
inline constexpr ImU32 kAudio = IM_COL32(52, 140, 92, 255);
inline constexpr ImU32 kText = IM_COL32(140, 92, 196, 255);
inline constexpr ImU32 kSubtitle = IM_COL32(40, 150, 160, 255);
inline constexpr ImU32 kSolid = IM_COL32(110, 110, 120, 255);
inline constexpr ImU32 kAdjustment = IM_COL32(196, 128, 52, 255);
inline constexpr ImU32 kCompound = IM_COL32(52, 120, 140, 255);
inline constexpr ImU32 kImage = IM_COL32(90, 100, 170, 255);
inline constexpr ImU32 kPlayhead = IM_COL32(235, 70, 60, 255);
inline constexpr ImU32 kSnap = IM_COL32(255, 220, 60, 255);
inline constexpr ImU32 kSelection = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kOffline = IM_COL32(170, 40, 40, 255);
inline constexpr ImU32 kWorkArea = IM_COL32(90, 140, 220, 70);
inline constexpr ImU32 kKeyframe = IM_COL32(240, 200, 80, 255);
}  // namespace col

ImU32 clipColor(const Clip& c, TrackKind track, MediaKind media = MediaKind::Unknown);
ImU32 scaleColor(ImU32 c, float factor, float alpha = 1.0f);

enum class Icon {
    Play, Pause, Stop, ToStart, ToEnd, StepBack, StepForward, Loop, Keyframe, KeyframeFilled, KeyPrev, KeyNext,
    Plus, Minus, Close, Eye, EyeOff, Lock, Unlock, Speaker, Mute, Razor, Pointer, Magnet, Link, Gear, Folder,
    Trash, Marker, Up, Down, Reset, Search, Film, Music, Image, Text, Check, Warning,
    Save, Undo, Redo, Export, Subtitle, Effects, Palette, Help, Sound,
};
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);
// Square button with a vector icon; `active` draws it highlighted.
bool iconButton(const char* id, Icon icon, const char* tooltip = nullptr, bool active = false, float size = 0.0f);
bool textToggle(const char* label, bool* value, const char* tooltip = nullptr, ImU32 onColor = IM_COL32(70, 120, 200, 255));
// Toolbar button with a vector icon and a text label (more discoverable than icons alone).
bool iconTextButton(const char* id, Icon icon, const char* label, const char* tooltip = nullptr, bool active = false,
                    bool enabled = true);
// Form row with the label on the left (reads naturally for Japanese UI text);
// the next widget fills the rest of the row. Pass "##id" labels to widgets.
void formLabel(const char* label, float labelWidth = 0.0f);
// formLabel() + returns a hidden widget id for the same label, so it can wrap
// an existing call: ImGui::SliderFloat(formRow(tr("Gain")), ...).
const char* formRow(const char* label, float labelWidth = 0.0f);
void tooltip(const char* text);  // hover tooltip (respects the tooltip setting)
void setTooltipsEnabled(bool on);
void helpMarker(const char* text);

// Time display in the configured style ("timecode" / "frames" / "seconds").
void setTimeStyle(const std::string& style);
std::string formatTime(Time t, Rational fps);
std::string formatTimeAs(Time t, Rational fps, const std::string& style);
// Editable time field: shows the time, accepts timecode / frames / seconds input.
bool timeField(const char* id, Time& t, Rational fps, float width = 110.0f);

// Stereo peak/RMS meter (dBFS scale -60..+6).
void levelMeter(const char* id, const audio::Meter& m, ImVec2 size, bool vertical = true);
float linearToDb(float v);

// Keyframable parameter row.
struct ParamEdit {
    bool changed = false;          // value edited (apply `value` at the current time)
    ParamValue value{};
    bool toggleAnimation = false;  // stopwatch clicked
    bool addKey = false;
    bool removeKey = false;
    std::optional<Time> jumpTo;    // keyframe navigation (clip-local time)
    bool reset = false;
    bool editing = false;          // widget is being dragged (merge undo)
};
ParamEdit paramRow(const ParamDef& def, const AnimatedParam* param, Time local, bool showKeyframes = true);

// Section header used in the inspector.
bool sectionHeader(const char* label, bool defaultOpen = true);

}  // namespace avc::ui
