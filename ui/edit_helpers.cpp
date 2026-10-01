#include "ui/edit_helpers.h"

#include <algorithm>

#include "core/i18n.h"

namespace avc::ui {

const ParamSet* paramSetOf(const Clip& c, const ParamRef& r) {
    switch (r.target) {
    case ParamTarget::Transform: return &c.transform;
    case ParamTarget::Audio: return &c.audio;
    case ParamTarget::Text: return &c.textParams;
    case ParamTarget::Effect:
        for (const auto& e : c.effects)
            if (e.id == r.effect) return &e.params;
        return nullptr;
    }
    return nullptr;
}

ParamSet* paramSetOf(Clip& c, const ParamRef& r) { return const_cast<ParamSet*>(paramSetOf(static_cast<const Clip&>(c), r)); }

Time clipLocalTime(const Clip& c, Time playhead) {
    Time local = playhead - c.start;
    if (local.ticks < 0) local = Time{0};
    if (c.duration.ticks > 0 && local >= c.duration) local = c.duration - Time{1};
    return local;
}

Status setParamValue(App& app, const ParamRef& ref, ParamValue v, Time local, const std::string& mergeKey) {
    return app.editSequence("Change Parameter", [&](SequenceEditor& e) {
        if (!e.clip(ref.clip)) return Status::error("Clip not found");
        ParamSet* ps = paramSetOf(e.mutableClip(ref.clip), ref);
        if (!ps) return Status::error("Effect not found");
        AnimatedParam& p = ps->getOrAdd(ref.param, v);
        if (p.animated()) p.setValueAt(local, v);
        else p.setStatic(v);
        return Status::ok();
    }, EditOptions{mergeKey});
}

void applyParamEdit(App& app, const ParamRef& ref, const ParamDef& def, const ParamEdit& ed, Time local) {
    const std::string key = "param:" + std::to_string(ref.clip) + ":" + std::to_string(ref.effect) + ":" + ref.param;
    if (ed.changed) setParamValue(app, ref, ed.value, local, key);
    if (ed.reset)
        app.editSequence("Reset Parameter", [&](SequenceEditor& e) {
            ParamSet* ps = paramSetOf(e.mutableClip(ref.clip), ref);
            if (!ps) return Status::error("Effect not found");
            ps->setStatic(ref.param, def.def);
            return Status::ok();
        });
    if (ed.toggleAnimation)
        app.editSequence("Toggle Keyframes", [&](SequenceEditor& e) {
            ParamSet* ps = paramSetOf(e.mutableClip(ref.clip), ref);
            if (!ps) return Status::error("Effect not found");
            AnimatedParam& p = ps->getOrAdd(ref.param, def.def);
            if (p.animated()) {
                const ParamValue now = p.evaluate(local);
                p.setStatic(now);
            } else {
                p.addKey(Keyframe{local, p.staticValue()});
            }
            return Status::ok();
        });
    if (ed.addKey || ed.removeKey)
        app.editSequence(ed.addKey ? "Add Keyframe" : "Remove Keyframe", [&](SequenceEditor& e) {
            ParamSet* ps = paramSetOf(e.mutableClip(ref.clip), ref);
            if (!ps) return Status::error("Effect not found");
            AnimatedParam& p = ps->getOrAdd(ref.param, def.def);
            if (ed.addKey) p.addKey(Keyframe{local, p.evaluate(local)});
            else p.removeKeyAt(local, Time::fromMilliseconds(1));
            return Status::ok();
        });
    if (ed.jumpTo) {
        if (const Sequence* s = app.sequence())
            if (const Clip* c = s->clip(ref.clip)) app.seek(c->start + *ed.jumpTo);
    }
}

}  // namespace avc::ui
