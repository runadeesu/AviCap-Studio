#pragma once
// Text style presets (Title, Pop Caption, Lower Third, Neon, Gradient,
// Simple): used by the Library's text page to add a styled text clip and by
// the Inspector to restyle the selected one.

#include <functional>
#include <vector>

#include <imgui.h>

#include "timeline/model.h"

namespace avc::ui {

struct TextPreset {
    const char* name;    // English label (tr)
    const char* sample;  // preview text (tr)
    std::function<void(Clip&, const Sequence&)> apply;
    std::function<void(ImDrawList*, ImVec2 a, ImVec2 b, const char* text)> preview;
};

const std::vector<TextPreset>& textPresets();

// Restores the default look (size, weight, alignment, colours, stroke,
// shadow, glow, gradient, background, position) so a preset applied to an
// existing clip does not mix with the previous style. Text and timing stay.
void resetTextLook(Clip& clip);

}  // namespace avc::ui
