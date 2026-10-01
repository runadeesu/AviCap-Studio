#pragma once
// Keyframe-aware parameter edits shared by the inspector, colour panel and
// viewer handles. Every change is an undoable App edit; continuous drags use
// a merge key so they collapse into one undo step.

#include <string>

#include "ui/app.h"
#include "ui/widgets.h"

namespace avc::ui {

enum class ParamTarget { Transform, Audio, Text, Effect };

struct ParamRef {
    ClipId clip = kInvalidId;
    ParamTarget target = ParamTarget::Transform;
    Id effect = kInvalidId;  // for ParamTarget::Effect
    std::string param;
};

const ParamSet* paramSetOf(const Clip& c, const ParamRef& r);
ParamSet* paramSetOf(Clip& c, const ParamRef& r);

// Sets the value at the clip-local time (adds/updates a key when animated).
Status setParamValue(App& app, const ParamRef& ref, ParamValue v, Time local, const std::string& mergeKey);
// Applies the result of a paramRow() widget.
void applyParamEdit(App& app, const ParamRef& ref, const ParamDef& def, const ParamEdit& edit, Time local);

// Clip-local time of the playhead for a clip (clamped into the clip).
Time clipLocalTime(const Clip& c, Time playhead);

}  // namespace avc::ui
