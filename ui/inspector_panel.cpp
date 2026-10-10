// Inspector: properties of the selected clip (timing, transform, opacity and
// blend, text, speed, audio, transitions and the effect stack, all keyframable)
// or of the sequence when nothing is selected.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <set>

#include "core/i18n.h"
#include "effects/effects.h"
#include "text/text_raster.h"
#include "timeline/edit_ops.h"
#include "ui/edit_helpers.h"
#include "ui/panels.h"
#include "ui/text_presets.h"
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

// Small muted caption above a field.
void caption(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
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
        // Pill: "N clips selected (showing the first)".
        char buf[160];
        std::snprintf(buf, sizeof buf, "%zu %s", app.selection.clips.size(), tr("clips selected"));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const float fs = ImGui::GetFontSize();
        const ImVec2 size(std::min(ts.x + fs * 2.2f, ImGui::GetContentRegionAvail().x), fs + 10);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, p + size, pal::kAccentSoft, size.y * 0.5f);
        drawIcon(dl, Icon::Info, p + ImVec2(fs * 0.85f, size.y * 0.5f), fs * 0.8f, pal::kAccentText);
        dl->PushClipRect(p, p + size, true);
        dl->AddText(p + ImVec2(fs * 1.6f, (size.y - ts.y) * 0.5f), pal::kAccentText, buf);
        dl->PopClipRect();
        ImGui::Dummy(size);
        tooltip(tr("clips selected (showing the first)"));
        ImGui::Spacing();
    }
    drawClip(app, *seq, *clip);
}

void InspectorPanel::drawSequence(App& app, const Sequence& seq) {
    headingText(tr("Project Settings"), 1.1f);
    ImGui::Spacing();
    char name[128];
    std::snprintf(name, sizeof name, "%s", seq.name.c_str());
    if (ImGui::InputText(formRow(tr("Name")), name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string n = name;
        app.editSequence("Rename Sequence", [&](SequenceEditor& e) {
            e.props().name = n;
            return Status::ok();
        });
    }
    // Aspect ratio presets (landscape, vertical for Shorts/TikTok, square...).
    formLabel(tr("Aspect Ratio"));
    ImGui::BeginGroup();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    int presetCount = 0;
    const AspectPreset* presets = aspectPresets(&presetCount);
    for (int i = 0; i < presetCount; ++i) {
        const AspectPreset& ap = presets[i];
        const bool on = ap.matches(seq.width, seq.height);
        if (i > 0) {
            const float w = ImGui::CalcTextSize(ap.label).x + ImGui::GetStyle().FramePadding.x * 1.8f;
            if (ImGui::GetItemRectMax().x + 6 + w <= right) ImGui::SameLine(0, 6);
        }
        if (chipButton(ap.label, on) && !on) app.setSequenceAspect(ap.num, ap.den);
        tooltip(tr(ap.hint));
    }
    ImGui::EndGroup();
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
    const std::string cur = fpsText(seq.frameRate) + " fps";
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
    if (ImGui::ColorEdit3(formRow(tr("Background")), bg, ImGuiColorEditFlags_NoInputs)) {
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

    // Overview card: label left (muted), value right.
    ImGui::Spacing();
    beginCard("##overview");
    auto row = [](const char* label, const std::string& value) {
        const float x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextMuted);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(x0 + avail - ImGui::CalcTextSize(value.c_str()).x);
        ImGui::TextUnformatted(value.c_str());
    };
    row(tr("Duration"), formatTime(seq.duration(), seq.frameRate));
    row(tr("Clips"), std::to_string(seq.clipCount()));
    row(tr("Resolution"), std::to_string(seq.width) + " × " + std::to_string(seq.height));
    if (seq.workArea)
        row(tr("In/Out"), formatTime(seq.workArea->start, seq.frameRate) + " - " + formatTime(seq.workArea->end(), seq.frameRate));
    endCard();
    ImGui::Spacing();
    hintText(tr("Select a clip in the timeline to edit its properties."));
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
    // One-click looks (the same presets as the Library's text page).
    if (clip.kind == ClipKind::Text) {
        ImGui::Spacing();
        caption(tr("Style"));
        const auto& presets = textPresets();
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = avail > ImGui::GetFontSize() * 15 ? 3 : 2;
        const float gap = 6.0f;
        const float w = (avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float h = ImGui::GetFontSize() * 2.6f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < presets.size(); ++i) {
            if (i % static_cast<size_t>(cols) != 0) ImGui::SameLine(0, gap);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(static_cast<int>(i));
            const bool pressed = ImGui::InvisibleButton("##style", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            tooltip(tr(presets[i].name));
            dl->AddRectFilled(p, p + ImVec2(w, h), hovered ? IM_COL32(52, 50, 72, 255) : IM_COL32(36, 36, 50, 255), pal::kControlRadius);
            dl->PushClipRect(p, p + ImVec2(w, h), true);
            presets[i].preview(dl, p, p + ImVec2(w, h), tr(presets[i].name));
            dl->PopClipRect();
            if (hovered) dl->AddRect(p, p + ImVec2(w, h), pal::kAccent, pal::kControlRadius, 1.5f);
            if (pressed) app.applyTextPreset(clip.id, i);
        }
        ImGui::Spacing();
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
    formLabel(tr("Italic"));
    changed |= toggleSwitch("##italic", &st.italic);
    if (changed) {
        const ClipId id = clip.id;
        app.editSequence("Text Style", [&](SequenceEditor& e) {
            e.mutableClip(id).textStyle = st;
            return Status::ok();
        }, EditOptions{"textstyle:" + std::to_string(id)});
    }
    for (const char* group : {"Fill", "Stroke", "Shadow", "Background", "Glow", "Animation", "Layout"})
        if (sectionHeader(tr(group), std::strcmp(group, "Fill") == 0)) drawParamSet(app, clip, textParamDefs(), 2, group);
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
        beginCard("##fxcard");
        const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        bool enabled = inst.enabled;
        if (toggleSwitch("##en", &enabled)) {
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
        ImGui::SameLine(rightX - fh * 3 - 6);
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
        endCard();
        ImGui::PopID();
        ImGui::Spacing();
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
    if (clip.effects.empty()) {
        hintText(audio ? tr("No audio effects yet. Add one below or drag one from the Library.")
                       : tr("No effects yet. Add one below or drag one from the Library."));
        ImGui::Spacing();
    }
    ImGui::SetNextItemWidth(-1);
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

// ------------------------------------------------------------------ clip

namespace {

enum InspectorPage { kPageText, kPageBasic, kPageAudio, kPageSpeed, kPageEffects, kPageTransitions };

Icon clipIcon(const Clip& clip, bool audio, const MediaItem* media) {
    switch (clip.kind) {
    case ClipKind::Text: return Icon::Text;
    case ClipKind::Subtitle: return Icon::Subtitle;
    case ClipKind::Solid: return Icon::Palette;
    case ClipKind::Adjustment: return Icon::Effects;
    default: break;
    }
    if (audio) return Icon::Music;
    if (media && media->info.kind == MediaKind::Image) return Icon::Image;
    return Icon::Film;
}

}  // namespace

void InspectorPanel::drawClipHeader(App& app, const Sequence& seq, const Clip& clip) {
    const Track* track = seq.trackOfClip(clip.id);
    const bool audio = track && !isVisualTrack(track->kind);
    const MediaItem* media = clip.kind == ClipKind::Media ? app.project().findMedia(clip.media) : nullptr;
    const float fs = ImGui::GetFontSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Kind tile (clip colour + icon), then the editable name and a meta line.
    const float tile = std::round(fs * 2.6f);
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    const ImU32 color = clipColor(clip, track ? track->kind : TrackKind::Video, media ? media->info.kind : MediaKind::Unknown);
    dl->AddRectFilled(tp, tp + ImVec2(tile, tile), color, pal::kControlRadius + 1);
    dl->AddRectFilled(tp, tp + ImVec2(tile, tile * 0.5f), IM_COL32(255, 255, 255, 26), pal::kControlRadius + 1, ImDrawFlags_RoundCornersTop);
    drawIcon(dl, clipIcon(clip, audio, media), tp + ImVec2(tile * 0.5f, tile * 0.5f), fs * 1.2f, IM_COL32(255, 255, 255, 240));
    ImGui::Dummy(ImVec2(tile, tile));
    ImGui::SameLine(0, 10);
    ImGui::BeginGroup();
    char name[256];
    std::snprintf(name, sizeof name, "%s", clip.name.c_str());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
    ImFont* bold = boldFont();
    if (bold) ImGui::PushFont(bold, 0.0f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string n = name;
        const ClipId id = clip.id;
        app.editSequence("Rename Clip", [&](SequenceEditor& e) {
            e.mutableClip(id).name = n;
            return Status::ok();
        });
    }
    if (bold) ImGui::PopFont();
    tooltip(tr("Click to rename"));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    std::string meta = tr(clipKindName(clip.kind));
    if (track) meta += std::string("  ·  ") + track->name;
    if (media && media->name != clip.name) meta += std::string("  ·  ") + media->name;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextMuted);
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(ImGui::GetContentRegionAvail().x, fs * 2), true);
    ImGui::TextUnformatted(meta.c_str());
    ImGui::PopClipRect();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // Start / duration side by side, captions above the fields.
    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    Time start = clip.start, dur = clip.duration;
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    caption(tr("Start"));
    if (timeField("##start", start, seq.frameRate, half)) {
        const std::set<ClipId> ids = {clip.id};
        const Time delta = start - clip.start;
        app.editSequence("Move", [&](SequenceEditor& e) { return edit::moveClips(e, ids, delta, 0, 0); });
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(rowPos.x + half + 8, rowPos.y));
    ImGui::BeginGroup();
    caption(tr("Duration"));
    if (timeField("##dur", dur, seq.frameRate, half) && dur.ticks > 0) {
        const ClipId id = clip.id;
        const Time end = clip.start + dur;
        app.editSequence("Trim", [&](SequenceEditor& e) { return edit::trimClip(e, id, edit::Edge::Tail, end, false); });
    }
    ImGui::EndGroup();
}

void InspectorPanel::drawClip(App& app, const Sequence& seq, const Clip& clip) {
    const Track* track = seq.trackOfClip(clip.id);
    const bool visual = track && isVisualTrack(track->kind);
    const bool audioClip = track && !visual;
    drawClipHeader(app, seq, clip);
    ImGui::Spacing();

    // Pages (tabs) depend on the clip type; the last chosen page is kept when
    // the next clip has it too.
    std::vector<std::pair<int, const char*>> pages;
    if (clip.kind == ClipKind::Text || clip.kind == ClipKind::Subtitle) pages.push_back({kPageText, tr("Text")});
    if (visual && clip.kind != ClipKind::Adjustment) pages.push_back({kPageBasic, tr("Basic")});
    if (audioClip) pages.push_back({kPageAudio, tr("Audio")});
    if (clip.hasLimitedSource()) pages.push_back({kPageSpeed, tr("Speed")});
    if (clip.kind != ClipKind::Subtitle) pages.push_back({kPageEffects, tr("Effects")});
    pages.push_back({kPageTransitions, tr("Transitions")});
    int current = 0;
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i].first == page_) current = static_cast<int>(i);
    std::vector<const char*> labels;
    for (const auto& p : pages) labels.push_back(p.second);
    if (underlineTabs("##inspectorpages", labels.data(), static_cast<int>(labels.size()), &current)) page_ = pages[static_cast<size_t>(current)].first;
    const int page = pages[static_cast<size_t>(current)].first;
    ImGui::Spacing();

    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None);
    switch (page) {
    case kPageText: drawTextSection(app, clip); break;
    case kPageBasic:
        if (sectionHeader(tr("Transform"))) {
            drawParamSet(app, clip, transformParamDefs(), 0, "Transform");
            bool fh = clip.flipH, fv = clip.flipV;
            bool flipChanged = checkbox(tr("Flip Horizontal"), &fh);  // evaluated in order (layout)
            ImGui::SameLine();
            flipChanged |= checkbox(tr("Flip Vertical"), &fv);
            if (flipChanged) {
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
        if (clip.kind == ClipKind::Solid && sectionHeader(tr("Color Matte"))) {
            float c[4] = {clip.solidColor[0], clip.solidColor[1], clip.solidColor[2], clip.solidColor[3]};
            if (ImGui::ColorEdit4(formRow(tr("Color")), c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                const ClipId id = clip.id;
                const ParamValue v{c[0], c[1], c[2], c[3]};
                app.editSequence("Matte Color", [&](SequenceEditor& e) {
                    e.mutableClip(id).solidColor = v;
                    return Status::ok();
                }, EditOptions{"solid:" + std::to_string(id)});
            }
        }
        if (sectionHeader(tr("Crop"), false)) drawParamSet(app, clip, transformParamDefs(), 0, "Crop");
        break;
    case kPageAudio: {
        if (sectionHeader(tr("Basic"))) {
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
        break;
    }
    case kPageSpeed: drawSpeed(app, clip); break;
    case kPageEffects: drawEffects(app, seq, clip, audioClip); break;
    case kPageTransitions: drawTransitions(app, seq, clip); break;
    default: break;
    }
    ImGui::EndChild();
}

void InspectorPanel::drawSpeed(App& app, const Clip& clip) {
    // Preset chips (the common speeds), then an exact value.
    caption(tr("Speed"));
    const double cur = clip.speed.toDouble();
    static const std::pair<const char*, Rational> presets[] = {{"0.25x", {1, 4}}, {"0.5x", {1, 2}}, {"1x", {1, 1}},
                                                               {"1.5x", {3, 2}},  {"2x", {2, 1}},   {"4x", {4, 1}}};
    for (size_t i = 0; i < std::size(presets); ++i) {
        if (i > 0) ImGui::SameLine(0, 6);
        if (chipButton(presets[i].first, std::abs(presets[i].second.toDouble() - cur) < 1e-6) && std::abs(presets[i].second.toDouble() - cur) > 1e-6) {
            const ClipId id = clip.id;
            const Rational r = presets[i].second;
            const bool rp = speedRipple_;
            app.editSequence("Change Speed", [&](SequenceEditor& e) { return edit::setClipSpeed(e, id, r, rp); });
        }
    }
    ImGui::Spacing();
    float pct = static_cast<float>(cur * 100.0);
    ImGui::InputFloat(formRow(tr("Speed (%)")), &pct, 10.0f, 50.0f, "%.1f %%");
    if (ImGui::IsItemDeactivatedAfterEdit() && pct > 0.5f) {
        const Rational r{static_cast<int64_t>(std::lround(pct * 10)), 1000};
        const ClipId id = clip.id;
        const bool rp = speedRipple_;
        app.editSequence("Change Speed", [&](SequenceEditor& e) { return edit::setClipSpeed(e, id, r.reduced(), rp); });
    }
    const Time srcDur{static_cast<int64_t>(static_cast<double>(clip.duration.ticks) * cur)};
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextMuted);
    ImGui::Text("%s %s  →  %s", tr("Length:"), formatTime(srcDur, app.sequence()->frameRate).c_str(),
                formatTime(clip.duration, app.sequence()->frameRate).c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    toggleSwitch("##ripple", &speedRipple_);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Ripple"));
    tooltip(tr("Ripple edit (shift following clips)"));
    bool rev = clip.reverse;
    if (toggleSwitch("##reverse", &rev)) {
        const ClipId id = clip.id;
        app.editSequence("Reverse", [&](SequenceEditor& e) { return edit::setClipReverse(e, id, rev); });
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Reverse"));
}

void InspectorPanel::drawTransitions(App& app, const Sequence& seq, const Clip& clip) {
    hintText(tr("Drag a transition from the Library onto a clip edge, or choose one here."));
    ImGui::Spacing();
    for (int side = 0; side < 2; ++side) {
        const std::optional<TransitionSpec>& tsp = side == 0 ? clip.transitionIn : clip.transitionOut;
        ImGui::PushID(side);
        beginCard(side == 0 ? "##in" : "##out");
        headingText(side == 0 ? tr("In") : tr("Out"), 0.95f);
        const fx::TransitionDef* cur = tsp ? fx::findTransition(tsp->type) : nullptr;
        ImGui::SetNextItemWidth(-1);
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
            Time d = tsp->duration;
            formLabel(tr("Duration"), ImGui::GetFontSize() * 6);
            if (timeField("##tdur", d, seq.frameRate, -1) && d.ticks > 0) {
                const ClipId id = clip.id;
                const Time dd = minTime(d, clip.duration);
                app.editSequence("Transition Duration", [&](SequenceEditor& e) {
                    auto& opt = side == 0 ? e.mutableClip(id).transitionIn : e.mutableClip(id).transitionOut;
                    if (opt) opt->duration = dd;
                    return Status::ok();
                });
            }
        }
        endCard();
        ImGui::PopID();
        ImGui::Spacing();
    }
}

}  // namespace avc::ui
