// Inspector: properties of the selected clip (timing, transform, opacity and
// blend, text, speed, audio, transitions and the effect stack, all keyframable)
// or of the sequence when nothing is selected.

#include <algorithm>
#include <cstring>
#include <set>

#include "core/i18n.h"
#include "effects/effects.h"
#include "text/text_raster.h"
#include "timeline/edit_ops.h"
#include "ui/edit_helpers.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

int resizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

bool inputTextMultiline(const char* label, std::string& s, ImVec2 size) {
    return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, resizeCallback, &s);
}

const std::vector<std::string>& fontFamilies() {
    static std::vector<std::string> families = [] {
        std::set<std::string> set;
        for (const auto& f : text::systemFonts()) set.insert(f.family);
        return std::vector<std::string>(set.begin(), set.end());
    }();
    return families;
}

}  // namespace

void InspectorPanel::draw(App& app) {
    const Sequence* seq = app.sequence();
    if (!seq) {
        ImGui::TextDisabled("%s", tr("No sequence"));
        return;
    }
    if (app.selection.clips.empty()) {
        drawSequence(app, *seq);
        return;
    }
    const Clip* clip = app.primaryClip();
    if (!clip) return;
    if (app.selection.clips.size() > 1) {
        ImGui::TextDisabled("%zu %s", app.selection.clips.size(), tr("clips selected (showing the first)"));
        ImGui::Separator();
    }
    drawClip(app, *seq, *clip);
}

void InspectorPanel::drawSequence(App& app, const Sequence& seq) {
    ImGui::TextUnformatted(tr("Sequence"));
    ImGui::Separator();
    char name[128];
    std::snprintf(name, sizeof name, "%s", seq.name.c_str());
    if (ImGui::InputText(formRow(tr("Name")), name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string n = name;
        app.editSequence("Rename Sequence", [&](SequenceEditor& e) {
            e.props().name = n;
            return Status::ok();
        });
    }
    int wh[2] = {seq.width, seq.height};
    if (ImGui::InputInt2(formRow(tr("Resolution")), wh, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const int w = std::clamp(wh[0] & ~1, 16, 16384), h = std::clamp(wh[1] & ~1, 16, 16384);
        app.editSequence("Sequence Resolution", [&](SequenceEditor& e) {
            e.props().width = w;
            e.props().height = h;
            return Status::ok();
        });
    }
    static const std::pair<const char*, Rational> rates[] = {{"23.976", {24000, 1001}}, {"24", {24, 1}},   {"25", {25, 1}},
                                                             {"29.97", {30000, 1001}},  {"30", {30, 1}},   {"50", {50, 1}},
                                                             {"59.94", {60000, 1001}},  {"60", {60, 1}},   {"120", {120, 1}}};
    const std::string cur = fpsText(seq.frameRate);
    if (ImGui::BeginCombo(formRow(tr("Frame Rate")), cur.c_str())) {
        for (const auto& [label, r] : rates)
            if (ImGui::Selectable(label, r == seq.frameRate)) {
                const Rational rr = r;
                app.editSequence("Sequence Frame Rate", [&](SequenceEditor& e) {
                    e.props().frameRate = rr;
                    return Status::ok();
                });
            }
        ImGui::EndCombo();
    }
    float bg[4] = {seq.backgroundColor[0], seq.backgroundColor[1], seq.backgroundColor[2], seq.backgroundColor[3]};
    if (ImGui::ColorEdit3(formRow(tr("Background")), bg)) {
        const ParamValue v{bg[0], bg[1], bg[2], 1.0f};
        app.editSequence("Background Color", [&](SequenceEditor& e) {
            e.props().backgroundColor = v;
            return Status::ok();
        }, EditOptions{"seq-bg"});
    }
    float master = seq.masterVolumeDb;
    if (ImGui::SliderFloat(formRow(tr("Master Volume")), &master, -60.0f, 12.0f, "%.1f dB")) {
        app.editSequence("Master Volume", [&](SequenceEditor& e) {
            e.props().masterVolumeDb = master;
            return Status::ok();
        }, EditOptions{"seq-master"});
    }
    ImGui::Text("%s: %s", tr("Duration"), formatTime(seq.duration(), seq.frameRate).c_str());
    ImGui::Text("%s: %zu", tr("Clips"), seq.clipCount());
    if (seq.workArea)
        ImGui::Text("%s: %s - %s", tr("In/Out"), formatTime(seq.workArea->start, seq.frameRate).c_str(),
                    formatTime(seq.workArea->end(), seq.frameRate).c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Select a clip in the timeline to edit its properties."));
}

void InspectorPanel::drawParamSet(App& app, const Clip& clip, const std::vector<ParamDef>& defs, int which, const char* groupFilter) {
    const ParamTarget target = which == 0 ? ParamTarget::Transform : which == 1 ? ParamTarget::Audio : ParamTarget::Text;
    const ParamSet& ps = which == 0 ? clip.transform : which == 1 ? clip.audio : clip.textParams;
    const Time local = clipLocalTime(clip, app.playhead());
    for (const ParamDef& d : defs) {
        if (groupFilter && d.group != groupFilter) continue;
        const ParamEdit ed = paramRow(d, ps.find(d.id), local);
        applyParamEdit(app, ParamRef{clip.id, target, kInvalidId, d.id}, d, ed, local);
    }
}

void InspectorPanel::drawTextSection(App& app, const Clip& clip) {
    if (textFor_ != clip.id) {
        textFor_ = clip.id;
        textBuf_ = clip.text;
    }
    if (!ImGui::IsAnyItemActive() && textBuf_ != clip.text) textBuf_ = clip.text;  // undo/redo or external edits
    textBuf_.reserve(256);
    if (inputTextMultiline("##text", textBuf_, ImVec2(-1, ImGui::GetFontSize() * 4.5f))) {
        const std::string t = textBuf_;
        const ClipId id = clip.id;
        app.editSequence("Edit Text", [&](SequenceEditor& e) {
            Clip& c = e.mutableClip(id);
            c.text = t;
            if (c.kind == ClipKind::Text) c.name = t.substr(0, t.find('\n')).substr(0, 40);
            return Status::ok();
        }, EditOptions{"text:" + std::to_string(id)});
    }
    TextStyle st = clip.textStyle;
    bool changed = false;
    if (ImGui::BeginCombo(formRow(tr("Font")), st.fontFamily.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const auto& fam : fontFamilies())
            if (ImGui::Selectable(fam.c_str(), fam == st.fontFamily)) {
                st.fontFamily = fam;
                changed = true;
            }
        ImGui::EndCombo();
    }
    changed |= ImGui::DragFloat(formRow(tr("Size")), &st.fontSize, 0.5f, 4.0f, 600.0f, "%.0f px");
    changed |= ImGui::SliderInt(formRow(tr("Weight")), &st.fontWeight, 100, 900);
    int align = static_cast<int>(st.align);
    const char* aligns[] = {tr("Left"), tr("Center"), tr("Right")};
    if (ImGui::Combo(formRow(tr("Align")), &align, aligns, 3)) {
        st.align = static_cast<TextAlign>(align);
        changed = true;
    }
    changed |= ImGui::DragFloat(formRow(tr("Tracking")), &st.tracking, 1.0f, -200.0f, 1000.0f, "%.0f");
    changed |= ImGui::DragFloat(formRow(tr("Line Spacing")), &st.lineSpacing, 0.01f, 0.5f, 4.0f, "%.2f");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 9.0f);
    changed |= ImGui::Checkbox(tr("Italic"), &st.italic);
    if (changed) {
        const ClipId id = clip.id;
        app.editSequence("Text Style", [&](SequenceEditor& e) {
            e.mutableClip(id).textStyle = st;
            return Status::ok();
        }, EditOptions{"textstyle:" + std::to_string(id)});
    }
    for (const char* group : {"Fill", "Stroke", "Shadow", "Background", "Glow", "Animation", "Layout"}) {
        if (ImGui::TreeNodeEx(tr(group), ImGuiTreeNodeFlags_SpanAvailWidth | (std::strcmp(group, "Fill") == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))) {
            drawParamSet(app, clip, textParamDefs(), 2, group);
            ImGui::TreePop();
        }
    }
}

void InspectorPanel::drawEffects(App& app, const Sequence& seq, const Clip& clip, bool audio) {
    (void)seq;
    const Time local = clipLocalTime(clip, app.playhead());
    const auto& reg = fx::EffectRegistry::instance();
    int moveFrom = -1, moveTo = -1, removeAt = -1;
    for (size_t i = 0; i < clip.effects.size(); ++i) {
        const EffectInstance& inst = clip.effects[i];
        const fx::EffectDef* def = reg.find(inst.effectId);
        ImGui::PushID(static_cast<int>(inst.id));
        bool enabled = inst.enabled;
        if (ImGui::Checkbox("##en", &enabled)) {
            const ClipId cid = clip.id;
            const Id eid = inst.id;
            app.editSequence(enabled ? "Enable Effect" : "Disable Effect", [&](SequenceEditor& e) {
                for (auto& ef : e.mutableClip(cid).effects)
                    if (ef.id == eid) ef.enabled = enabled;
                return Status::ok();
            });
        }
        tooltip(tr("Enable / bypass this effect"));
        ImGui::SameLine();
        const bool open = ImGui::TreeNodeEx("##fx", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                            "%s", def ? tr(def->name.c_str()) : inst.effectId.c_str());
        const float fh = ImGui::GetFrameHeight() * 0.8f;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - fh * 3 - 6);
        if (iconButton("##up", Icon::Up, tr("Move up"), false, fh) && i > 0) {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        ImGui::SameLine(0, 2);
        if (iconButton("##down", Icon::Down, tr("Move down"), false, fh) && i + 1 < clip.effects.size()) {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        ImGui::SameLine(0, 2);
        if (iconButton("##remove", Icon::Trash, tr("Remove effect"), false, fh)) removeAt = static_cast<int>(i);
        if (open) {
            if (!def) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", tr("Effect not available (missing plugin?)"));
            } else {
                for (const ParamDef& d : def->params) {
                    const ParamEdit ed = paramRow(d, inst.params.find(d.id), local);
                    applyParamEdit(app, ParamRef{clip.id, ParamTarget::Effect, inst.id, d.id}, d, ed, local);
                }
                if (inst.effectId == "color.lut") {
                    auto it = inst.properties.find("file");
                    ImGui::TextDisabled("%s", it == inst.properties.end() || it->second.empty() ? tr("No LUT file") : it->second.c_str());
                    if (ImGui::Button(tr("Load .cube LUT..."))) {
                        auto files = app.dialogs().openFiles(tr("Load LUT"), {{"3D LUT (.cube)", "*.cube"}}, false);
                        if (!files.empty()) {
                            std::string err;
                            if (!fx::loadCubeLut(files.front(), &err)) {
                                app.notify(LogLevel::Warning, std::string(tr("LUT could not be loaded")) + ": " + err);
                            } else {
                                const ClipId cid = clip.id;
                                const Id eid = inst.id;
                                const std::string path = files.front();
                                app.editSequence("Load LUT", [&](SequenceEditor& e) {
                                    for (auto& ef : e.mutableClip(cid).effects)
                                        if (ef.id == eid) ef.properties["file"] = path;
                                    return Status::ok();
                                });
                            }
                        }
                    }
                } else if (inst.effectId == "color.curves") {
                    if (ImGui::Button(tr("Edit curves in the Color panel"))) app.ui().showColor = true;
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (moveFrom >= 0 || removeAt >= 0) {
        const ClipId cid = clip.id;
        app.editSequence(removeAt >= 0 ? "Remove Effect" : "Reorder Effects", [&](SequenceEditor& e) {
            auto& fx = e.mutableClip(cid).effects;
            if (removeAt >= 0 && removeAt < static_cast<int>(fx.size())) fx.erase(fx.begin() + removeAt);
            else if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(fx.size())) std::swap(fx[static_cast<size_t>(moveFrom)], fx[static_cast<size_t>(moveTo)]);
            return Status::ok();
        });
    }
    // Add effect
    if (ImGui::BeginCombo("##addfx", tr("+ Add Effect"), ImGuiComboFlags_HeightLarge)) {
        std::string lastCat;
        for (const fx::EffectDef* d : reg.list(audio ? fx::EffectKind::Audio : fx::EffectKind::Video)) {
            if (d->category != lastCat) {
                ImGui::SeparatorText(tr(d->category.c_str()));
                lastCat = d->category;
            }
            if (ImGui::Selectable(tr(d->name.c_str()))) app.applyEffectToClip(clip.id, d->id);
        }
        ImGui::EndCombo();
    }
}

void InspectorPanel::drawClip(App& app, const Sequence& seq, const Clip& clip) {
    const Track* track = seq.trackOfClip(clip.id);
    const bool visual = track && isVisualTrack(track->kind);
    const MediaItem* media = clip.kind == ClipKind::Media ? app.project().findMedia(clip.media) : nullptr;

    // Header: name and timing
    char name[256];
    std::snprintf(name, sizeof name, "%s", clip.name.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string n = name;
        const ClipId id = clip.id;
        app.editSequence("Rename Clip", [&](SequenceEditor& e) {
            e.mutableClip(id).name = n;
            return Status::ok();
        });
    }
    ImGui::TextDisabled("%s  |  %s", tr(clipKindName(clip.kind)), track ? track->name.c_str() : "");
    if (media) ImGui::TextDisabled("%s", media->name.c_str());
    Time start = clip.start, dur = clip.duration;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Start"));
    ImGui::SameLine(ImGui::GetFontSize() * 5);
    if (timeField("##start", start, seq.frameRate)) {
        const std::set<ClipId> ids = {clip.id};
        const Time delta = start - clip.start;
        app.editSequence("Move", [&](SequenceEditor& e) { return edit::moveClips(e, ids, delta, 0, 0); });
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(tr("Duration"));
    ImGui::SameLine();
    if (timeField("##dur", dur, seq.frameRate) && dur.ticks > 0) {
        const ClipId id = clip.id;
        const Time end = clip.start + dur;
        app.editSequence("Trim", [&](SequenceEditor& e) { return edit::trimClip(e, id, edit::Edge::Tail, end, false); });
    }

    if (visual && clip.kind != ClipKind::Adjustment) {
        if (sectionHeader(tr("Transform"))) {
            drawParamSet(app, clip, transformParamDefs(), 0, "Transform");
            bool fh = clip.flipH, fv = clip.flipV;
            if (ImGui::Checkbox(tr("Flip Horizontal"), &fh) | ImGui::Checkbox(tr("Flip Vertical"), &fv)) {
                const ClipId id = clip.id;
                app.editSequence("Flip", [&](SequenceEditor& e) {
                    e.mutableClip(id).flipH = fh;
                    e.mutableClip(id).flipV = fv;
                    return Status::ok();
                });
            }
        }
        if (sectionHeader(tr("Opacity & Blend"))) {
            drawParamSet(app, clip, transformParamDefs(), 0, "Opacity");
            int blend = static_cast<int>(clip.blend);
            if (ImGui::BeginCombo(formRow(tr("Blend Mode")), tr(blendModeName(clip.blend)))) {
                for (int b = 0; b < static_cast<int>(BlendMode::Count); ++b)
                    if (ImGui::Selectable(tr(blendModeName(static_cast<BlendMode>(b))), b == blend)) {
                        const ClipId id = clip.id;
                        const BlendMode m = static_cast<BlendMode>(b);
                        app.editSequence("Blend Mode", [&](SequenceEditor& e) {
                            e.mutableClip(id).blend = m;
                            return Status::ok();
                        });
                    }
                ImGui::EndCombo();
            }
        }
        if (sectionHeader(tr("Crop"), false)) drawParamSet(app, clip, transformParamDefs(), 0, "Crop");
    }
    if (clip.kind == ClipKind::Text || clip.kind == ClipKind::Subtitle) {
        if (sectionHeader(tr("Text"))) drawTextSection(app, clip);
    }
    if (clip.kind == ClipKind::Solid && sectionHeader(tr("Color Matte"))) {
        float c[4] = {clip.solidColor[0], clip.solidColor[1], clip.solidColor[2], clip.solidColor[3]};
        if (ImGui::ColorEdit4(formRow(tr("Color")), c)) {
            const ClipId id = clip.id;
            const ParamValue v{c[0], c[1], c[2], c[3]};
            app.editSequence("Matte Color", [&](SequenceEditor& e) {
                e.mutableClip(id).solidColor = v;
                return Status::ok();
            }, EditOptions{"solid:" + std::to_string(id)});
        }
    }
    if (clip.hasLimitedSource() && sectionHeader(tr("Speed"), false)) {
        float pct = static_cast<float>(clip.speed.toDouble() * 100.0);
        static bool ripple = true;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::InputFloat("%##speed", &pct, 10.0f, 50.0f, "%.1f");
        if (ImGui::IsItemDeactivatedAfterEdit() && pct > 0.5f) {
            const Rational r{static_cast<int64_t>(std::lround(pct * 10)), 1000};
            const ClipId id = clip.id;
            const bool rp = ripple;
            app.editSequence("Change Speed", [&](SequenceEditor& e) { return edit::setClipSpeed(e, id, r.reduced(), rp); });
        }
        ImGui::SameLine();
        ImGui::Checkbox(tr("Ripple"), &ripple);
        bool rev = clip.reverse;
        if (ImGui::Checkbox(tr("Reverse"), &rev)) {
            const ClipId id = clip.id;
            app.editSequence("Reverse", [&](SequenceEditor& e) { return edit::setClipReverse(e, id, rev); });
        }
    }
    const bool audioClip = track && !isVisualTrack(track->kind);
    if (audioClip && sectionHeader(tr("Audio"))) {
        drawParamSet(app, clip, audioParamDefs(), 1, nullptr);
        float gain = clip.gainDb;
        if (ImGui::DragFloat(formRow(tr("Gain")), &gain, 0.1f, -40.0f, 40.0f, "%.1f dB")) {
            const ClipId id = clip.id;
            app.editSequence("Clip Gain", [&](SequenceEditor& e) {
                e.mutableClip(id).gainDb = gain;
                return Status::ok();
            }, EditOptions{"gain:" + std::to_string(id)});
        }
    }
    if (sectionHeader(tr("Transitions"), false)) {
        for (int side = 0; side < 2; ++side) {
            const std::optional<TransitionSpec>& tsp = side == 0 ? clip.transitionIn : clip.transitionOut;
            ImGui::PushID(side);
            ImGui::TextUnformatted(side == 0 ? tr("In") : tr("Out"));
            ImGui::SameLine(ImGui::GetFontSize() * 3);
            const fx::TransitionDef* cur = tsp ? fx::findTransition(tsp->type) : nullptr;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
            if (ImGui::BeginCombo("##type", tsp ? (cur ? tr(cur->name.c_str()) : tsp->type.c_str()) : tr("None"))) {
                if (ImGui::Selectable(tr("None"), !tsp)) {
                    const ClipId id = clip.id;
                    app.editSequence("Remove Transition", [&](SequenceEditor& e) {
                        (side == 0 ? e.mutableClip(id).transitionIn : e.mutableClip(id).transitionOut).reset();
                        return Status::ok();
                    });
                }
                for (const auto& t : fx::transitions())
                    if (ImGui::Selectable(tr(t.name.c_str()), tsp && tsp->type == t.id)) {
                        const ClipId id = clip.id;
                        const std::string type = t.id;
                        const Time d = tsp ? tsp->duration : minTime(Time::fromSeconds(t.defaultSeconds), Time{clip.duration.ticks / 2});
                        app.editSequence("Set Transition", [&](SequenceEditor& e) {
                            TransitionSpec s;
                            s.type = type;
                            s.duration = d;
                            (side == 0 ? e.mutableClip(id).transitionIn : e.mutableClip(id).transitionOut) = s;
                            return Status::ok();
                        });
                    }
                ImGui::EndCombo();
            }
            if (tsp) {
                ImGui::SameLine();
                Time d = tsp->duration;
                if (timeField("##tdur", d, seq.frameRate, ImGui::GetFontSize() * 6) && d.ticks > 0) {
                    const ClipId id = clip.id;
                    const Time dd = minTime(d, clip.duration);
                    app.editSequence("Transition Duration", [&](SequenceEditor& e) {
                        auto& opt = side == 0 ? e.mutableClip(id).transitionIn : e.mutableClip(id).transitionOut;
                        if (opt) opt->duration = dd;
                        return Status::ok();
                    });
                }
            }
            ImGui::PopID();
        }
    }
    if (clip.kind != ClipKind::Subtitle && sectionHeader(audioClip ? tr("Audio Effects") : tr("Effects"))) drawEffects(app, seq, clip, audioClip);
}

}  // namespace avc::ui
