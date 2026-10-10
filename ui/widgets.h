#pragma once
// Shared drawing helpers: colours, vector icons (independent of font
// coverage), timecode fields, level meters and the keyframable parameter
// editor used by the inspector, effects and colour panels.

#include <optional>
#include <string>

#include <imgui.h>

#include "audio/mixer.h"
#include "timeline/model.h"
#include "ui/style.h"

namespace avc::ui {

namespace col {
inline constexpr ImU32 kVideo = pal::kClipVideo;
inline constexpr ImU32 kAudio = pal::kClipAudio;
inline constexpr ImU32 kText = pal::kClipText;
inline constexpr ImU32 kSubtitle = pal::kClipSubtitle;
inline constexpr ImU32 kSolid = pal::kClipSolid;
inline constexpr ImU32 kAdjustment = pal::kClipAdjust;
inline constexpr ImU32 kCompound = pal::kClipCompound;
inline constexpr ImU32 kImage = pal::kClipImage;
inline constexpr ImU32 kPlayhead = pal::kPlayhead;
inline constexpr ImU32 kSnap = IM_COL32(255, 214, 72, 255);
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
    Sparkle, Transition, Grid, List, Fullscreen, Star, StarFilled, More, Info, Bolt, Volume,
};
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);
// Square button with a vector icon; `active` draws it highlighted.
bool iconButton(const char* id, Icon icon, const char* tooltip = nullptr, bool active = false, float size = 0.0f);
bool textToggle(const char* label, bool* value, const char* tooltip = nullptr, ImU32 onColor = IM_COL32(70, 120, 200, 255));
// Toolbar button with a vector icon and a text label (more discoverable than icons alone).
bool iconTextButton(const char* id, Icon icon, const char* label, const char* tooltip = nullptr, bool active = false,
                    bool enabled = true);
// ------------------------------------------------------------------ modern controls
// Accent-filled call-to-action button (Export, Create, Apply...).
bool primaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
// Transparent button with an icon and text; highlighted on hover (toolbars).
bool ghostButton(const char* id, Icon icon, const char* label, const char* tooltip = nullptr, bool active = false, bool enabled = true);
// Pill-shaped segmented control; returns true when the selection changed.
bool segmented(const char* id, const char* const* labels, int count, int* current, const char* const* tooltips = nullptr);
// Vertical navigation rail item (icon above a short label).
bool railButton(const char* id, Icon icon, const char* label, bool active, float width);
// Rounded filter chip.
bool chipButton(const char* label, bool active);
// Semibold UI font (nullptr when none was found; see loadUiFonts).
ImFont* boldFont();
// Section heading in the bold UI font.
void headingText(const char* text, float scale = 1.0f);
// Modern replacements for ImGui::Checkbox (accent-filled box with a check
// mark) and an on/off switch for enabling whole features.
bool checkbox(const char* label, bool* v);
bool toggleSwitch(const char* id, bool* v, const char* tooltip = nullptr);
// Text tabs with an accent underline (inspector pages).
bool underlineTabs(const char* id, const char* const* labels, int count, int* current);
// Icon-over-label tab (library page bar); the active one is accent coloured.
bool iconTab(const char* id, Icon icon, const char* label, bool active, ImVec2 size);
// Frame shapes offered in the viewer and the project settings.
struct AspectPreset {
    const char* label;  // "16:9"
    int num, den;
    const char* hint;  // English (tr)
    [[nodiscard]] bool matches(int w, int h) const { return static_cast<long long>(w) * den == static_cast<long long>(h) * num; }
};
const AspectPreset* aspectPresets(int* count);
// Full-width sidebar entry (icon + label) used by the settings categories.
bool navItem(const char* id, Icon icon, const char* label, bool active);
// Soft drop shadow and hairline border around a floating (undocked) window;
// call right after Begin().
void floatingWindowDecor();
// Row of round accent-colour swatches; `accent` holds the preset id.
bool accentSwatches(const char* id, std::string& accent);
// Rounded "card" background behind the next items (call endCard() after).
void beginCard(const char* id, ImVec2 size = ImVec2(0, 0), ImU32 bg = pal::kPanelRaised);
void endCard();
// Vertical fader (groove + accent fill + knob); `value` in [minV, maxV].
bool vFader(const char* id, float* value, float minV, float maxV, ImVec2 size);
// Thin horizontal slider with a round knob (pan, zoom). `centered` fills from the middle.
bool slimSlider(const char* id, float* value, float minV, float maxV, float width, bool centered = false);
// Small coloured dot (status indicators).
void statusDot(ImU32 color);

// Form row with the label on the left (reads naturally for Japanese UI text);
// the next widget fills the rest of the row. Pass "##id" labels to widgets.
void formLabel(const char* label, float labelWidth = 0.0f);
// formLabel() + returns a hidden widget id for the same label, so it can wrap
// an existing call: ImGui::SliderFloat(formRow(tr("Gain")), ...).
const char* formRow(const char* label, float labelWidth = 0.0f);
void tooltip(const char* text);  // hover tooltip (respects the tooltip setting)
void setTooltipsEnabled(bool on);
void helpMarker(const char* text);
// Dimmed hint text that wraps to the available width (Japanese has no spaces,
// so long hints must wrap instead of being clipped).
void hintText(const char* text);

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
