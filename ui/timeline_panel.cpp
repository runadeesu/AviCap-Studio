// Timeline panel: ruler, track headers, virtualized clip lanes and all mouse
// editing. Drags never touch the document until release: every frame the
// pending operation is applied to a scratch copy of the project (cheap thanks
// to structural sharing) and drawn as a live preview; release commits the very
// same operation through App so it becomes one undo step.

#include <algorithm>
#include <cmath>
#include <cstring>

#include <imgui_internal.h>

#include "core/i18n.h"
#include "effects/effects.h"
#include "timeline/edit_ops.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {

constexpr float kEdgePx = 6.0f;
constexpr float kNameBar = 16.0f;
constexpr float kDividerH = 6.0f;

float laneDefaultHeight(const Track& t, float overrideH = 0.0f) {
    const float scale = ImGui::GetFontSize() / 15.0f;
    if (overrideH > 0) return overrideH * scale;
    if (t.height > 0) return t.height * scale;
    if (t.kind == TrackKind::Audio) return 48.0f * scale;
    if (t.kind == TrackKind::Subtitle || t.kind == TrackKind::Text) return 34.0f * scale;
    return 56.0f * scale;
}

std::string trackLabel(const Sequence& seq, int index) {
    const Track& t = *seq.tracks[static_cast<size_t>(index)];
    return t.name.empty() ? std::string(trackKindName(t.kind)) : t.name;
}

int familyPos(const Sequence& seq, int trackIndex) {
    const bool audio = !isVisualTrack(seq.tracks[static_cast<size_t>(trackIndex)]->kind);
    const auto fam = audio ? seq.audioTrackIndices() : seq.visualTrackIndices();
    for (size_t i = 0; i < fam.size(); ++i)
        if (fam[i] == trackIndex) return static_cast<int>(i);
    return -1;
}

double niceStep(double minSeconds, double fps) {
    static const double steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    const double frame = 1.0 / std::max(1.0, fps);
    for (int f : {1, 2, 5, 10})
        if (frame * f >= minSeconds && frame * f < 1.0) return frame * f;
    for (double s : steps)
        if (s >= minSeconds) return s;
    return 14400;
}

}  // namespace

float TimelinePanel::timeToX(Time t) const { return areaMin_.x + static_cast<float>((t.seconds() - scroll_) * pps_); }

Time TimelinePanel::xToTime(float x) const {
    const double s = scroll_ + (x - areaMin_.x) / pps_;
    return Time::fromSeconds(std::max(0.0, s));
}

Time TimelinePanel::snapThreshold() const { return Time::fromSeconds(8.0 / std::max(1e-6, pps_)); }

const TimelinePanel::Lane* TimelinePanel::laneForTrack(int track) const {
    for (const auto& l : lanes_)
        if (l.track == track) return &l;
    return nullptr;
}

int TimelinePanel::laneAt(float y) const {
    for (size_t i = 0; i < lanes_.size(); ++i)
        if (y >= lanes_[i].y && y < lanes_[i].y + lanes_[i].h) return static_cast<int>(i);
    return -1;
}

void TimelinePanel::layoutLanes(const Sequence& seq, float top, float width) {
    (void)width;
    lanes_.clear();
    float y = top - scrollY_;
    const auto vis = seq.visualTrackIndices();
    auto height = [&](int index) {
        const Track& t = *seq.tracks[static_cast<size_t>(index)];
        return laneDefaultHeight(t, drag_.kind == DragKind::TrackHeight && drag_.track == index ? drag_.newHeight : 0.0f);
    };
    for (auto it = vis.rbegin(); it != vis.rend(); ++it) {
        const float h = height(*it);
        lanes_.push_back({*it, y, h, false});
        y += h + 1;
    }
    y += kDividerH;
    for (int i : seq.audioTrackIndices()) {
        const float h = height(i);
        lanes_.push_back({i, y, h, true});
        y += h + 1;
    }
    contentH_ = y + scrollY_ - top + 40.0f;
}

TimelinePanel::Hit TimelinePanel::hitTest(const Sequence& seq, ImVec2 p) const {
    Hit h;
    h.lane = laneAt(p.y);
    if (h.lane < 0 || p.x < areaMin_.x || p.x > areaMax_.x) return h;
    h.track = lanes_[static_cast<size_t>(h.lane)].track;
    const Track& t = *seq.tracks[static_cast<size_t>(h.track)];
    const Time at = xToTime(p.x);
    // Prefer edges of neighbouring clips within the grab margin.
    const Time margin = Time::fromSeconds(kEdgePx / pps_);
    const size_t first = t.lowerBound(at - margin);
    for (size_t i = first; i < t.clips.size(); ++i) {
        const Clip& c = *t.clips[i];
        if (c.start > at + margin) break;
        const float x0 = timeToX(c.start), x1 = timeToX(c.end());
        const bool wide = x1 - x0 > kEdgePx * 3;
        if (std::fabs(p.x - x0) <= kEdgePx && (wide || p.x <= x0 + kEdgePx)) {
            h.clip = &c;
            h.zone = Hit::Head;
            if (p.x >= x0) return h;  // inside the clip near its head wins
        } else if (std::fabs(p.x - x1) <= kEdgePx && (wide || p.x >= x1 - kEdgePx)) {
            if (!h.clip || p.x <= x1) {
                h.clip = &c;
                h.zone = Hit::Tail;
            }
        } else if (p.x >= x0 && p.x < x1 && !h.clip) {
            h.clip = &c;
            h.zone = Hit::Body;
        }
    }
    return h;
}

std::optional<TimelinePanel::DropTarget> TimelinePanel::dropTargetAt(const App& app, ImVec2 screen) const {
    const Sequence* seq = app.sequence();
    if (!seq || screen.x < areaMin_.x || screen.x > areaMax_.x || screen.y < areaMin_.y || screen.y > areaMax_.y) return std::nullopt;
    DropTarget d;
    d.time = xToTime(screen.x).snappedToFrame(seq->frameRate);
    d.videoTrack = app.targetVideoTrackIndex();
    d.audioTrack = app.targetAudioTrackIndex();
    const int li = laneAt(screen.y);
    if (li >= 0) {
        const Lane& l = lanes_[static_cast<size_t>(li)];
        if (l.audio) d.audioTrack = l.track;
        else if (seq->tracks[static_cast<size_t>(l.track)]->kind == TrackKind::Video) d.videoTrack = l.track;
    }
    return d;
}

// ------------------------------------------------------------------ draw

void TimelinePanel::draw(App& app) {
    const Sequence* active = app.sequence();
    if (!active) {
        ImGui::TextDisabled("%s", tr("No sequence"));
        return;
    }
    drawToolbar(app);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##timeline_area", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    const float fs = ImGui::GetFontSize();
    headerW_ = fs * 11.0f;
    rulerH_ = fs * 2.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    areaMin_ = ImVec2(origin.x + headerW_, origin.y + rulerH_);
    areaMax_ = ImVec2(origin.x + avail.x, origin.y + avail.y);
    const float areaW = std::max(10.0f, areaMax_.x - areaMin_.x);

    // The drag preview (if any) is what we draw.
    const Sequence* seq = active;
    if (preview_) {
        if (const Sequence* p = preview_->findSequence(app.sequenceId())) seq = p;
    }
    applyZoomRequests(app, *active);
    layoutLanes(*seq, areaMin_.y, areaW);
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentH_ - (areaMax_.y - areaMin_.y)));
    layoutLanes(*seq, areaMin_.y, areaW);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(areaMin_, areaMax_, IM_COL32(28, 29, 33, 255));

    // Lanes input first (so drags update the preview before drawing).
    handleLaneInput(app, *active);
    if (preview_) {
        if (const Sequence* p = preview_->findSequence(app.sequenceId())) seq = p;
    } else {
        seq = app.sequence();
    }
    if (!seq) {
        ImGui::EndChild();
        return;
    }

    dl->PushClipRect(areaMin_, areaMax_, true);
    // Lane backgrounds
    for (const Lane& l : lanes_) {
        if (l.y > areaMax_.y || l.y + l.h < areaMin_.y) continue;
        const Track& t = *seq->tracks[static_cast<size_t>(l.track)];
        ImU32 bg = l.audio ? IM_COL32(33, 37, 35, 255) : IM_COL32(34, 35, 41, 255);
        if (t.locked) bg = IM_COL32(40, 34, 34, 255);
        dl->AddRectFilled(ImVec2(areaMin_.x, l.y), ImVec2(areaMax_.x, l.y + l.h), bg);
    }
    // Work area shading
    if (seq->workArea) {
        const float x0 = timeToX(seq->workArea->start), x1 = timeToX(seq->workArea->end());
        dl->AddRectFilled(ImVec2(x0, areaMin_.y), ImVec2(x1, areaMax_.y), IM_COL32(90, 140, 220, 18));
    }
    drawClips(app, *seq, dl);

    // Marquee
    if (drag_.kind == DragKind::Marquee && drag_.moved) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImVec2 a(std::min(m.x, drag_.startMouse.x), std::min(m.y, drag_.startMouse.y));
        const ImVec2 b(std::max(m.x, drag_.startMouse.x), std::max(m.y, drag_.startMouse.y));
        dl->AddRectFilled(a, b, IM_COL32(120, 160, 255, 40));
        dl->AddRect(a, b, IM_COL32(120, 160, 255, 200));
    }
    if (snapLine_) {
        const float x = timeToX(*snapLine_);
        dl->AddLine(ImVec2(x, areaMin_.y), ImVec2(x, areaMax_.y), col::kSnap, 1.5f);
    }
    if (razorLine_) {
        const float x = timeToX(*razorLine_);
        dl->AddLine(ImVec2(x, areaMin_.y), ImVec2(x, areaMax_.y), IM_COL32(255, 255, 255, 160), 1.0f);
    }
    // Playhead
    const float px = timeToX(app.playhead());
    dl->AddLine(ImVec2(px, areaMin_.y), ImVec2(px, areaMax_.y), col::kPlayhead, 1.5f);
    // Audio/video divider
    for (size_t i = 0; i + 1 < lanes_.size(); ++i)
        if (!lanes_[i].audio && lanes_[i + 1].audio) {
            const float y = lanes_[i].y + lanes_[i].h + kDividerH * 0.5f;
            dl->AddLine(ImVec2(areaMin_.x, y), ImVec2(areaMax_.x, y), IM_COL32(80, 80, 90, 255), 2.0f);
        }
    dl->PopClipRect();

    drawRuler(app, *seq, dl, ImVec2(areaMin_.x, origin.y), ImVec2(areaMax_.x, areaMin_.y));
    drawHeaders(app, *seq, dl);

    // Corner: timecode of the playhead.
    dl->AddRectFilled(origin, ImVec2(areaMin_.x, areaMin_.y), IM_COL32(24, 24, 28, 255));
    const std::string tc = formatTime(app.playhead(), seq->frameRate);
    dl->AddText(ImVec2(origin.x + 8, origin.y + (rulerH_ - fs) * 0.5f), IM_COL32(235, 235, 235, 255), tc.c_str());

    // Mouse wheel: zoom (Ctrl), horizontal scroll (Shift), vertical scroll.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
        const float mx = std::clamp(io.MousePos.x, areaMin_.x, areaMax_.x);
        if (io.KeyCtrl && io.MouseWheel != 0) {
            const double t = scroll_ + (mx - areaMin_.x) / pps_;
            pps_ = std::clamp(pps_ * std::pow(1.25, io.MouseWheel), 0.002, seq->frameRate.toDouble() * 80.0);
            scroll_ = std::max(0.0, t - (mx - areaMin_.x) / pps_);
        } else if (io.KeyShift || io.MouseWheelH != 0) {
            const float w = io.MouseWheelH != 0 ? io.MouseWheelH : io.MouseWheel;
            scroll_ = std::max(0.0, scroll_ - w * 80.0 / pps_);
        } else {
            scrollY_ -= io.MouseWheel * fs * 3.0f;
        }
    }
    // Auto-scroll while dragging near the edges.
    if (drag_.kind != DragKind::None && drag_.kind != DragKind::TrackHeight && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float edge = 30.0f;
        if (io.MousePos.x > areaMax_.x - edge) scroll_ += (io.MousePos.x - (areaMax_.x - edge)) * 0.15 / pps_ * 10.0;
        else if (io.MousePos.x < areaMin_.x + edge && scroll_ > 0)
            scroll_ = std::max(0.0, scroll_ - ((areaMin_.x + edge) - io.MousePos.x) * 0.15 / pps_ * 10.0);
    }
    // Follow the playhead during playback (page scrolling).
    if (app.playing()) {
        const float x = timeToX(app.playhead());
        if (x > areaMax_.x - 10 || x < areaMin_.x) scroll_ = std::max(0.0, app.playhead().seconds() - areaW * 0.1 / pps_);
    }
    // Horizontal scrollbar substitute: thin overview at the bottom.
    {
        const double total = std::max(seq->duration().seconds() + 10.0, (areaW / pps_) + scroll_);
        const float y = areaMax_.y - 5;
        const float a = areaMin_.x + static_cast<float>(scroll_ / total) * areaW;
        const float b = areaMin_.x + static_cast<float>((scroll_ + areaW / pps_) / total) * areaW;
        dl->AddRectFilled(ImVec2(areaMin_.x, y), ImVec2(areaMax_.x, areaMax_.y), IM_COL32(20, 20, 22, 255));
        dl->AddRectFilled(ImVec2(a, y), ImVec2(std::max(a + 8, b), areaMax_.y), IM_COL32(110, 110, 120, 255), 2.0f);
    }

    clipContextMenu(app);
    emptyContextMenu(app);
    ImGui::EndChild();
}

void TimelinePanel::applyZoomRequests(App& app, const Sequence& seq) {
    UiState& ui = app.ui();
    const float areaW = std::max(10.0f, areaMax_.x - areaMin_.x);
    if (fitPending_ || ui.zoomToFit) {
        const double dur = std::max(5.0, seq.duration().seconds() * 1.05);
        pps_ = std::clamp(areaW / dur, 0.002, seq.frameRate.toDouble() * 80.0);
        scroll_ = 0;
        fitPending_ = false;
        ui.zoomToFit = false;
    }
    if (ui.zoomSteps != 0) {
        const double t = app.playhead().seconds();
        const float px = timeToX(app.playhead());
        const bool visible = px >= areaMin_.x && px <= areaMax_.x;
        const float anchor = visible ? px : areaMin_.x + areaW * 0.5f;
        const double anchorT = visible ? t : scroll_ + (anchor - areaMin_.x) / pps_;
        pps_ = std::clamp(pps_ * std::pow(1.5, ui.zoomSteps), 0.002, seq.frameRate.toDouble() * 80.0);
        scroll_ = std::max(0.0, anchorT - (anchor - areaMin_.x) / pps_);
        ui.zoomSteps = 0;
    }
}

void TimelinePanel::drawToolbar(App& app) {
    const float s = ImGui::GetFrameHeight();
    struct T {
        Tool tool;
        Icon icon;
        const char* action;
    };
    static const T tools[] = {{Tool::Select, Icon::Pointer, "tool.select"}, {Tool::Razor, Icon::Razor, "tool.razor"},
                              {Tool::Ripple, Icon::ToEnd, "tool.ripple"},   {Tool::Roll, Icon::Link, "tool.roll"},
                              {Tool::Slip, Icon::Film, "tool.slip"},       {Tool::Slide, Icon::StepForward, "tool.slide"}};
    for (const T& t : tools) {
        ImGui::PushID(t.action);
        std::string tip = std::string(tr(toolLabel(t.tool)));
        const std::string sc = app.commands().shortcutText(t.action);
        if (!sc.empty()) tip += " (" + sc + ")";
        if (iconButton("##tool", t.icon, tip.c_str(), app.tool == t.tool, s)) app.tool = t.tool;
        ImGui::PopID();
        ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 10);
    if (iconButton("##snap", Icon::Magnet, tr("Snapping (S)"), app.snapping, s)) app.commands().run("timeline.snapping");
    ImGui::SameLine(0, 2);
    if (iconButton("##linked", Icon::Link, tr("Linked Selection"), app.linkedSelection, s)) app.linkedSelection = !app.linkedSelection;
    ImGui::SameLine(0, 10);
    if (iconButton("##split", Icon::Razor, tr("Split at Playhead (Ctrl+K)"), false, s)) app.splitAtPlayhead(false);
    ImGui::SameLine(0, 2);
    if (iconButton("##marker", Icon::Marker, tr("Add Marker (M)"), false, s)) app.addMarker();
    ImGui::SameLine(0, 2);
    if (iconButton("##title", Icon::Text, tr("Add Title (Ctrl+T)"), false, s)) app.addTextClip(tr("Title"));
    ImGui::SameLine(0, 10);
    if (iconButton("##zoomout", Icon::Minus, tr("Zoom Out (-)"), false, s)) --app.ui().zoomSteps;
    ImGui::SameLine(0, 2);
    if (iconButton("##zoomin", Icon::Plus, tr("Zoom In (=)"), false, s)) ++app.ui().zoomSteps;
    ImGui::SameLine(0, 2);
    if (ImGui::Button(tr("Fit"))) app.ui().zoomToFit = true;
    tooltip(tr("Zoom to Fit (Shift+Z)"));
    if (app.project().sequences.size() > 1) {
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        const Sequence* cur = app.sequence();
        if (ImGui::BeginCombo("##seq", cur ? cur->name.c_str() : "")) {
            for (const auto& sp : app.project().sequences)
                if (ImGui::Selectable(sp->name.c_str(), cur && sp->id == cur->id)) {
                    const SequenceId id = sp->id;
                    app.doc().edit("Open Sequence", [&](ProjectEditor& pe) {
                        pe.root().activeSequence = id;
                        return Status::ok();
                    }, EditOptions{"", false});
                    app.selection.clips.clear();
                }
            ImGui::EndCombo();
        }
        tooltip(tr("Sequence"));
    }
    if (const Sequence* seq = app.sequence()) {
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("%s  %dx%d  %s fps  %s %s", seq->name.c_str(), seq->width, seq->height, fpsText(seq->frameRate).c_str(),
                            tr("Duration"), formatTime(seq->duration(), seq->frameRate).c_str());
    }
}

// ------------------------------------------------------------------ ruler

void TimelinePanel::drawRuler(App& app, const Sequence& seq, ImDrawList* dl, ImVec2 min, ImVec2 max) {
    dl->AddRectFilled(min, max, IM_COL32(24, 24, 28, 255));
    const double fps = seq.frameRate.toDouble();
    const double major = niceStep(90.0 / pps_, fps);
    const double minor = major / (major >= 1.0 ? 5.0 : std::max(1.0, std::round(major * fps)));
    const double t0 = scroll_, t1 = scroll_ + (max.x - min.x) / pps_;
    dl->PushClipRect(min, max, true);
    if (seq.workArea) {
        const float x0 = timeToX(seq.workArea->start), x1 = timeToX(seq.workArea->end());
        dl->AddRectFilled(ImVec2(x0, max.y - 6), ImVec2(x1, max.y), IM_COL32(90, 140, 220, 200));
    }
    if (minor * pps_ >= 4.0)
        for (double t = std::floor(t0 / minor) * minor; t <= t1; t += minor) {
            const float x = min.x + static_cast<float>((t - scroll_) * pps_);
            dl->AddLine(ImVec2(x, max.y - 5), ImVec2(x, max.y), IM_COL32(110, 110, 115, 255));
        }
    for (double t = std::floor(t0 / major) * major; t <= t1 + major; t += major) {
        const float x = min.x + static_cast<float>((t - scroll_) * pps_);
        dl->AddLine(ImVec2(x, max.y - 11), ImVec2(x, max.y), IM_COL32(160, 160, 165, 255));
        const std::string label = formatTime(Time::fromSeconds(t + 1e-9), seq.frameRate);
        dl->AddText(ImVec2(x + 3, min.y + 2), IM_COL32(170, 170, 178, 255), label.c_str());
    }
    // Markers
    for (const Marker& m : seq.markers) {
        const float x = timeToX(m.time);
        if (x < min.x - 10 || x > max.x + 10) continue;
        const bool sel = app.selection.marker == m.id;
        if (m.duration.ticks > 0) {
            const float x1 = timeToX(m.time + m.duration);
            dl->AddRectFilled(ImVec2(x, max.y - 9), ImVec2(x1, max.y - 3), scaleColor(m.color, 1.0f, 0.6f));
        }
        drawIcon(dl, Icon::Marker, ImVec2(x, max.y - 9), 12.0f, sel ? IM_COL32(255, 255, 255, 255) : m.color);
    }
    // Playhead head
    const float px = timeToX(app.playhead());
    dl->AddTriangleFilled(ImVec2(px - 6, max.y - 12), ImVec2(px + 6, max.y - 12), ImVec2(px, max.y), col::kPlayhead);
    dl->AddLine(ImVec2(px, min.y), ImVec2(px, max.y), col::kPlayhead, 1.5f);
    dl->PopClipRect();

    // Interaction: markers (click/drag), scrubbing elsewhere.
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##ruler", ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
        drag_ = Drag{};
        for (const Marker& m : seq.markers)
            if (std::fabs(timeToX(m.time) - io.MousePos.x) <= 6 && io.MousePos.y > max.y - 18) {
                drag_.kind = DragKind::Marker;
                drag_.marker = m.id;
                drag_.markerOrig = m.time;
                drag_.startMouse = io.MousePos;
                app.selection.marker = m.id;
                break;
            }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && drag_.kind == DragKind::Marker) {
            editMarker_ = drag_.marker;
            for (const Marker& m : seq.markers)
                if (m.id == editMarker_) {
                    std::snprintf(markerName_, sizeof markerName_, "%s", m.name.c_str());
                    std::snprintf(markerComment_, sizeof markerComment_, "%s", m.comment.c_str());
                }
            ImGui::OpenPopup("##markeredit");
            drag_ = Drag{};
        }
    }
    if (ImGui::IsItemActive()) {
        if (drag_.kind == DragKind::Marker) {
            if (std::fabs(io.MousePos.x - drag_.startMouse.x) > 3) drag_.moved = true;
        } else {
            app.scrub(xToTime(io.MousePos.x));
        }
    }
    if (ImGui::IsItemDeactivated() && drag_.kind == DragKind::Marker) {
        if (drag_.moved) {
            const Time t = xToTime(io.MousePos.x).snappedToFrame(seq.frameRate);
            const MarkerId id = drag_.marker;
            app.editSequence("Move Marker", [&](SequenceEditor& e) {
                for (Marker m : e.seq().markers)
                    if (m.id == id) {
                        m.time = t;
                        return edit::updateMarker(e, m) ? Status::ok() : Status::error("Marker not found");
                    }
                return Status::error("Marker not found");
            });
        } else {
            app.seek(drag_.markerOrig);
        }
        drag_ = Drag{};
    }
    if (ImGui::BeginPopup("##markeredit")) {
        ImGui::TextUnformatted(tr("Marker"));
        ImGui::InputText(tr("Name"), markerName_, sizeof markerName_);
        ImGui::InputTextMultiline(tr("Comment"), markerComment_, sizeof markerComment_, ImVec2(0, ImGui::GetFontSize() * 4));
        if (ImGui::Button(tr("OK"))) {
            const MarkerId id = editMarker_;
            const std::string name = markerName_, comment = markerComment_;
            app.editSequence("Edit Marker", [&](SequenceEditor& e) {
                for (Marker m : e.seq().markers)
                    if (m.id == id) {
                        m.name = name;
                        m.comment = comment;
                        return edit::updateMarker(e, m) ? Status::ok() : Status::error("Marker not found");
                    }
                return Status::error("Marker not found");
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Delete"))) {
            const MarkerId id = editMarker_;
            app.editSequence("Delete Marker", [&](SequenceEditor& e) {
                return edit::removeMarker(e, id) ? Status::ok() : Status::error("Marker not found");
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------ headers

void TimelinePanel::drawHeaders(App& app, const Sequence& seq, ImDrawList* dl) {
    const ImVec2 hmin(areaMin_.x - headerW_, areaMin_.y), hmax(areaMin_.x, areaMax_.y);
    dl->AddRectFilled(hmin, hmax, IM_COL32(26, 26, 30, 255));
    ImGui::PushClipRect(hmin, hmax, true);
    const float s = ImGui::GetFontSize() * 1.25f;
    const int targetV = app.targetVideoTrackIndex(), targetA = app.targetAudioTrackIndex();
    for (const Lane& l : lanes_) {
        if (l.y > areaMax_.y || l.y + l.h < areaMin_.y) continue;
        const Track& t = *seq.tracks[static_cast<size_t>(l.track)];
        ImGui::PushID(static_cast<int>(t.id));
        const ImVec2 a(hmin.x, l.y), b(hmax.x - 1, l.y + l.h);
        dl->AddRectFilled(a, b, l.audio ? IM_COL32(36, 42, 39, 255) : IM_COL32(38, 39, 47, 255));
        // Target patch (sized to the track name, within limits)
        const bool targeted = l.track == targetV || l.track == targetA;
        const ImVec2 tp(a.x + 4, a.y + 4);
        const std::string label = trackLabel(seq, l.track);
        const float patchW = std::clamp(ImGui::CalcTextSize(label.c_str()).x + 8.0f, s * 1.6f, headerW_ - s * 3.6f - 12.0f);
        ImGui::SetCursorScreenPos(tp);
        if (ImGui::InvisibleButton("##target", ImVec2(patchW, s))) {
            const int pos = familyPos(seq, l.track);
            if (l.audio) app.targetAudioTrack = pos;
            else if (t.kind == TrackKind::Video) {
                int vp = 0;
                for (int i : seq.visualTrackIndices()) {
                    if (i == l.track) break;
                    if (seq.tracks[static_cast<size_t>(i)]->kind == TrackKind::Video) ++vp;
                }
                app.targetVideoTrack = vp;
            }
        }
        tooltip(tr("Target track for insert and paste"));
        dl->AddRectFilled(tp, tp + ImVec2(patchW, s), targeted ? IM_COL32(70, 120, 200, 255) : IM_COL32(60, 60, 68, 255), 3.0f);
        dl->PushClipRect(tp, tp + ImVec2(patchW, s), true);
        dl->AddText(tp + ImVec2(4, (s - ImGui::GetFontSize()) * 0.5f), IM_COL32(230, 230, 235, 255), label.c_str());
        dl->PopClipRect();

        // Toggles
        float x = tp.x + patchW + 6;
        auto toggleIcon = [&](const char* id, Icon on, Icon off, bool value, const char* tip) {
            ImGui::SetCursorScreenPos(ImVec2(x, tp.y));
            const bool pressed = iconButton(id, value ? on : off, tip, false, s);
            x += s + 2;
            return pressed;
        };
        if (!l.audio) {
            if (toggleIcon("##hide", Icon::EyeOff, Icon::Eye, t.hidden, tr("Toggle track output"))) {
                const TrackId id = t.id;
                app.editSequence("Toggle Track Output", [&](SequenceEditor& e) {
                    Track& mt = e.mutableTrackById(id);
                    mt.hidden = !mt.hidden;
                    return Status::ok();
                });
            }
        } else {
            if (toggleIcon("##mute", Icon::Mute, Icon::Speaker, t.muted, tr("Mute track"))) {
                const TrackId id = t.id;
                app.editSequence("Mute Track", [&](SequenceEditor& e) {
                    Track& mt = e.mutableTrackById(id);
                    mt.muted = !mt.muted;
                    return Status::ok();
                });
            }
            ImGui::SetCursorScreenPos(ImVec2(x, tp.y));
            bool solo = t.solo;
            if (textToggle("S", &solo, tr("Solo track"), IM_COL32(200, 160, 40, 255))) {
                const TrackId id = t.id;
                app.editSequence("Solo Track", [&](SequenceEditor& e) {
                    Track& mt = e.mutableTrackById(id);
                    mt.solo = !mt.solo;
                    return Status::ok();
                });
            }
            x += s + 2;
        }
        if (toggleIcon("##lock", Icon::Lock, Icon::Unlock, t.locked, tr("Lock track"))) {
            const TrackId id = t.id;
            app.editSequence("Lock Track", [&](SequenceEditor& e) {
                Track& mt = e.mutableTrackById(id);
                mt.locked = !mt.locked;
                return Status::ok();
            });
        }
        if (l.h > s * 2.2f) {
            const char* kind = tr(trackKindName(t.kind));
            dl->AddText(ImVec2(tp.x, tp.y + s + 4), IM_COL32(140, 140, 150, 255), kind);
        }
        // Context menu & rename
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + s + 6));
        ImGui::InvisibleButton("##hdr", ImVec2(std::max(1.0f, headerW_ - 6), std::max(1.0f, l.h - s - 8)));
        if (ImGui::BeginPopupContextItem("##trackctx")) {
            if (ImGui::MenuItem(tr("Add Video Track"))) app.addTrack(TrackKind::Video);
            if (ImGui::MenuItem(tr("Add Audio Track"))) app.addTrack(TrackKind::Audio);
            if (ImGui::MenuItem(tr("Add Subtitle Track"))) app.addTrack(TrackKind::Subtitle);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Rename..."))) {
                renameTrack_ = l.track;
                std::snprintf(renameBuf_, sizeof renameBuf_, "%s", t.name.c_str());
            }
            if (ImGui::MenuItem(tr("Delete Track"), nullptr, false, t.clips.empty())) {
                const TrackId id = t.id;
                app.editSequence("Delete Track", [&](SequenceEditor& e) { return edit::removeTrack(e, id); });
            }
            ImGui::EndPopup();
        }
        // Height resize grip
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y - 3));
        ImGui::InvisibleButton("##resize", ImVec2(headerW_, 6));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActivated()) {
            drag_ = Drag{};
            drag_.kind = DragKind::TrackHeight;
            drag_.track = l.track;
            drag_.trackHeight = l.h / (ImGui::GetFontSize() / 15.0f);
            drag_.startMouse = ImGui::GetIO().MousePos;
        }
        if (ImGui::IsItemActive() && drag_.kind == DragKind::TrackHeight && drag_.track == l.track)
            drag_.newHeight = std::clamp(
                drag_.trackHeight + (ImGui::GetIO().MousePos.y - drag_.startMouse.y) / (ImGui::GetFontSize() / 15.0f), 24.0f, 240.0f);
        if (ImGui::IsItemDeactivated() && drag_.kind == DragKind::TrackHeight && drag_.track == l.track) {
            const float nh = drag_.newHeight;
            const TrackId id = t.id;
            if (nh > 0)
                app.doc().editSequence("Track Height", app.sequenceId(), [&](SequenceEditor& e) {
                    e.mutableTrackById(id).height = nh;
                    return Status::ok();
                }, EditOptions{"", false});
            drag_ = Drag{};
        }
        dl->AddLine(ImVec2(a.x, b.y), ImVec2(hmax.x, b.y), IM_COL32(20, 20, 22, 255));
        ImGui::PopID();
    }
    ImGui::PopClipRect();

    if (renameTrack_ >= 0) {
        ImGui::OpenPopup("##renametrack");
        if (ImGui::BeginPopup("##renametrack")) {
            ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##name", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::Button(tr("OK"))) {
                const int ti = renameTrack_;
                const std::string name = renameBuf_;
                app.editSequence("Rename Track", [&](SequenceEditor& e) {
                    e.mutableTrack(ti).name = name;
                    return Status::ok();
                });
                renameTrack_ = -1;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                renameTrack_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            renameTrack_ = -1;
        }
    }
}

// ------------------------------------------------------------------ clips

void TimelinePanel::drawClips(App& app, const Sequence& seq, ImDrawList* dl) {
    const Time t0 = xToTime(areaMin_.x), t1 = xToTime(areaMax_.x);
    for (const Lane& l : lanes_) {
        if (l.y > areaMax_.y || l.y + l.h < areaMin_.y) continue;
        const Track& t = *seq.tracks[static_cast<size_t>(l.track)];
        for (size_t i = t.lowerBound(t0); i < t.clips.size(); ++i) {
            const Clip& c = *t.clips[i];
            if (c.start > t1) break;
            drawClip(app, seq, t, c, l, dl);
        }
    }
}

void TimelinePanel::drawClip(App& app, const Sequence& seq, const Track& track, const Clip& c, const Lane& lane, ImDrawList* dl) {
    (void)seq;
    float x0 = timeToX(c.start), x1 = timeToX(c.end());
    const float y0 = lane.y + 1, y1 = lane.y + lane.h - 1;
    if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;
    const float vx0 = std::max(x0, areaMin_.x - 2), vx1 = std::min(x1, areaMax_.x + 2);
    const MediaItem* media = c.kind == ClipKind::Media ? app.project().findMedia(c.media) : nullptr;
    const bool offline = media && app.mediaOffline(media->id);
    const bool selected = app.selection.clips.count(c.id) != 0;
    ImU32 base = clipColor(c, track.kind, media ? media->info.kind : MediaKind::Unknown);
    if (offline) base = col::kOffline;
    const float alpha = c.enabled ? 1.0f : 0.35f;
    const ImU32 body = scaleColor(base, selected ? 1.25f : 0.85f, alpha);
    const ImU32 bar = scaleColor(base, selected ? 1.05f : 0.65f, alpha);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), body, 3.0f);
    const float barH = std::min(kNameBar * ImGui::GetFontSize() / 15.0f, (y1 - y0) * 0.5f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + barH), bar, 3.0f, ImDrawFlags_RoundCornersTop);

    const float contentTop = y0 + barH;
    const float contentH = y1 - contentTop - 1;
    // Thumbnails (filmstrip) for video clips.
    if (media && !offline && isVisualTrack(track.kind) && media->info.hasVideo() && contentH >= 18 && x1 - x0 > 8) {
        const auto* v = media->info.primaryVideo();
        const float aspect = v && v->height > 0 ? static_cast<float>(v->width) / static_cast<float>(v->height) : 16.0f / 9.0f;
        const float tw = std::max(16.0f, contentH * aspect);
        int drawn = 0;
        const float start = std::max(x0, std::floor((vx0 - x0) / tw) * tw + x0);
        for (float x = start; x < vx1 && drawn < 64; x += tw, ++drawn) {
            const Time at = c.sourceTimeAt(xToTime(x));
            const ThumbnailRef th = app.assets().thumbnail(*media, at, 160);
            if (!th || th.texture == ImTextureID_Invalid) continue;
            const float w = std::min(tw, x1 - x);
            dl->AddImage(ImTextureRef(th.texture), ImVec2(x, contentTop), ImVec2(x + w, contentTop + contentH), ImVec2(0, 0),
                         ImVec2(w / tw, 1), IM_COL32(255, 255, 255, static_cast<int>(alpha * 230)));
        }
    }
    // Waveforms for audio clips.
    if (media && !offline && !isVisualTrack(track.kind) && media->info.hasAudio() && contentH >= 10) {
        if (auto peaks = app.assets().waveform(*media)) {
            const int cols = std::max(1, static_cast<int>((vx1 - vx0) / 2.0f));
            Time sa = c.sourceTimeAt(xToTime(vx0)), sb = c.sourceTimeAt(xToTime(vx1));
            const bool rev = c.reverse;
            if (rev) std::swap(sa, sb);
            if (c.freezeFrame || sb <= sa) sb = sa + Time::fromMilliseconds(1);
            const auto mm = peaks->query(sa.seconds(), sb.seconds(), cols, -1);
            const float gain = std::pow(10.0f, (c.audio.evaluate1("volume", Time{0}, 0.0f) + c.gainDb) / 20.0f);
            const float mid = contentTop + contentH * 0.5f;
            const float half = contentH * 0.48f;
            const ImU32 wc = scaleColor(base, 1.7f, alpha);
            for (int i = 0; i < static_cast<int>(mm.size()); ++i) {
                const auto& p = mm[static_cast<size_t>(rev ? mm.size() - 1 - static_cast<size_t>(i) : static_cast<size_t>(i))];
                const float x = vx0 + static_cast<float>(i) * 2.0f;
                const float lo = std::clamp(p.first * gain, -1.0f, 1.0f), hi = std::clamp(p.second * gain, -1.0f, 1.0f);
                dl->AddLine(ImVec2(x, mid - hi * half), ImVec2(x, mid - lo * half + 1), wc, 1.5f);
            }
        }
    }
    // Text preview for titles / subtitles.
    if ((c.kind == ClipKind::Text || c.kind == ClipKind::Subtitle) && contentH > ImGui::GetFontSize()) {
        dl->PushClipRect(ImVec2(vx0, contentTop), ImVec2(vx1, y1), true);
        dl->AddText(ImVec2(std::max(x0, areaMin_.x) + 4, contentTop + 2), IM_COL32(255, 255, 255, static_cast<int>(alpha * 220)),
                    c.text.c_str());
        dl->PopClipRect();
    }
    // Transitions
    auto drawTransition = [&](const TransitionSpec& tr, bool head) {
        const float w = static_cast<float>(tr.duration.seconds() * pps_);
        const float xa = head ? x0 : x1 - w, xb = head ? x0 + w : x1;
        dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), IM_COL32(255, 255, 255, 40));
        if (head) dl->AddLine(ImVec2(xa, y1), ImVec2(xb, y0), IM_COL32(255, 255, 255, 140), 1.5f);
        else dl->AddLine(ImVec2(xa, y0), ImVec2(xb, y1), IM_COL32(255, 255, 255, 140), 1.5f);
    };
    if (c.transitionIn && c.transitionIn->duration.ticks > 0) drawTransition(*c.transitionIn, true);
    if (c.transitionOut && c.transitionOut->duration.ticks > 0) drawTransition(*c.transitionOut, false);

    // Keyframe diamonds
    if (lane.h >= 36 && x1 - x0 > 20) {
        auto keys = [&](const ParamSet& ps) {
            for (const auto& [id, p] : ps.items())
                for (const auto& k : p.keys()) {
                    const float kx = timeToX(c.start + k.time);
                    if (kx >= x0 && kx <= x1) drawIcon(dl, Icon::KeyframeFilled, ImVec2(kx, y1 - 5), 8.0f, col::kKeyframe);
                }
        };
        keys(c.transform);
        keys(c.audio);
        keys(c.textParams);
        for (const auto& e : c.effects) keys(e.params);
    }

    // Name bar label + badges
    std::string label = c.name.empty() ? std::string(tr(clipKindName(c.kind))) : c.name;
    if (offline) label = std::string(tr("Media Offline")) + " - " + label;
    if (!c.effects.empty()) label += "  fx";
    if (c.speed != Rational{1, 1}) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "  %.0f%%", c.speed.toDouble() * 100.0);
        label += buf;
    }
    if (c.reverse) label += "  <<";
    if (c.freezeFrame) label += "  ||";
    if (!c.enabled) label += std::string("  (") + tr("Disabled") + ")";
    dl->PushClipRect(ImVec2(vx0, y0), ImVec2(std::max(vx0 + 1, vx1 - 2), y0 + barH), true);
    dl->AddText(ImVec2(std::max(x0, areaMin_.x) + 4, y0 + (barH - ImGui::GetFontSize()) * 0.5f),
                IM_COL32(255, 255, 255, static_cast<int>(alpha * 235)), label.c_str());
    dl->PopClipRect();

    if (selected) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col::kSelection, 3.0f, 2.0f);
    else dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 120), 3.0f);
    if (dropHighlight_ == c.id) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col::kSnap, 3.0f, 2.5f);
}

// ------------------------------------------------------------------ input

void TimelinePanel::handleLaneInput(App& app, const Sequence& seq) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(areaMin_);
    const ImVec2 size(std::max(1.0f, areaMax_.x - areaMin_.x), std::max(1.0f, areaMax_.y - areaMin_.y));
    ImGui::InvisibleButton("##lanes", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated();
    handleDrops(app, seq);  // must directly follow the item it targets
    const Hit hit = hovered ? hitTest(seq, io.MousePos) : Hit{};

    // Cursor feedback
    razorLine_.reset();
    if (hovered && drag_.kind == DragKind::None) {
        if (app.tool == Tool::Razor) {
            Time t = xToTime(io.MousePos.x).snappedToFrame(seq.frameRate);
            if (app.snapping) {
                auto pts = collectSnapPoints(seq, app.playhead(), {});
                auto sr = snapTime(pts, t, snapThreshold());
                if (sr.snapped) t = sr.time;
            }
            razorLine_ = t;
        } else if (hit.zone == Hit::Head || hit.zone == Hit::Tail) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }

    // Right click: context menus.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        contextTime_ = xToTime(io.MousePos.x).snappedToFrame(seq.frameRate);
        contextTrack_ = hit.track;
        if (hit.clip) {
            if (!app.selection.clips.count(hit.clip->id)) app.selectClip(hit.clip->id, false, false);
            contextClip_ = hit.clip->id;
            ImGui::OpenPopup("##clipctx");
        } else {
            contextClip_ = kInvalidId;
            ImGui::OpenPopup("##emptyctx");
        }
    }

    if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_ = Drag{};
        drag_.startMouse = io.MousePos;
        drag_.startTime = xToTime(io.MousePos.x);
        drag_.lane = hit.lane;
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
        const Track* track = hit.track >= 0 ? seq.tracks[static_cast<size_t>(hit.track)].get() : nullptr;
        if (app.tool == Tool::Razor) {
            Time t = razorLine_ ? *razorLine_ : xToTime(io.MousePos.x).snappedToFrame(seq.frameRate);
            if (shift) {
                app.editSequence("Split", [&](SequenceEditor& e) { return edit::splitAtTime(e, t, {}).status(); });
            } else if (hit.clip && track && !track->locked) {
                const ClipId id = hit.clip->id;
                const bool linked = app.linkedSelection;
                app.editSequence("Split", [&](SequenceEditor& e) { return edit::splitClip(e, id, t, linked).status(); });
            }
            drag_ = Drag{};
        } else if (hit.clip && track && !track->locked) {
            const ClipId id = hit.clip->id;
            drag_.clip = id;
            drag_.wasSelected = app.selection.clips.count(id) != 0;
            if (ctrl) app.selectClip(id, true, true);
            else if (shift) app.selectClip(id, true, false);
            else if (!drag_.wasSelected) app.selectClip(id, false, false);
            if (hit.zone == Hit::Head || hit.zone == Hit::Tail) {
                const bool head = hit.zone == Hit::Head;
                // Rolling edit: needs an adjacent clip on the other side of the cut.
                if (app.tool == Tool::Roll) {
                    const int idx = track->indexOf(id);
                    const int nidx = head ? idx - 1 : idx + 1;
                    if (nidx >= 0 && nidx < static_cast<int>(track->clips.size())) {
                        const Clip& n = *track->clips[static_cast<size_t>(nidx)];
                        if ((head && n.end() == hit.clip->start) || (!head && n.start == hit.clip->end())) {
                            drag_.kind = DragKind::Roll;
                            drag_.other = n.id;
                            if (head) std::swap(drag_.clip, drag_.other);  // clip = left, other = right
                        }
                    }
                }
                if (drag_.kind == DragKind::None) {
                    drag_.kind = head ? DragKind::TrimHead : DragKind::TrimTail;
                    drag_.ripple = app.tool == Tool::Ripple || ctrl;
                }
                std::set<ClipId> exclude = {id};
                for (ClipId l : seq.linkedClips(id)) exclude.insert(l);
                drag_.snaps = collectSnapPoints(seq, app.playhead(), exclude);
            } else if (app.tool == Tool::Slip) {
                drag_.kind = DragKind::Slip;
            } else if (app.tool == Tool::Slide) {
                drag_.kind = DragKind::Slide;
                drag_.snaps = collectSnapPoints(seq, app.playhead(), {id});
            } else {
                drag_.kind = DragKind::Move;
                drag_.clips = app.selection.clips;
                // Locked tracks never move.
                std::erase_if(drag_.clips, [&](ClipId cid) {
                    const Track* tt = seq.trackOfClip(cid);
                    return !tt || tt->locked;
                });
                Time lo = Time::max(), hi{0};
                for (ClipId cid : drag_.clips)
                    if (const Clip* cc = seq.clip(cid)) {
                        lo = minTime(lo, cc->start);
                        hi = maxTime(hi, cc->end());
                    }
                drag_.range = TimeRange{lo, hi - lo};
                drag_.snaps = collectSnapPoints(seq, app.playhead(), drag_.clips);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit.clip->kind == ClipKind::Compound) {
                const SequenceId nested = hit.clip->nested;
                app.doc().edit("Open Sequence", [&](ProjectEditor& pe) {
                    pe.root().activeSequence = nested;
                    return Status::ok();
                }, EditOptions{"", false});
                drag_ = Drag{};
            }
        } else {
            drag_.kind = DragKind::Marquee;
            drag_.marqueeBase = (ctrl || shift) ? app.selection.clips : std::set<ClipId>{};
        }
    }

    if (drag_.kind != DragKind::None && drag_.kind != DragKind::TrackHeight && drag_.kind != DragKind::Marker) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!drag_.moved && (std::fabs(io.MousePos.x - drag_.startMouse.x) > 3 || std::fabs(io.MousePos.y - drag_.startMouse.y) > 3))
                drag_.moved = true;
            if (drag_.moved) updateDrag(app, seq);
        } else {
            finishDrag(app);
        }
    }
}

void TimelinePanel::updateDrag(App& app, const Sequence& seq) {
    const ImGuiIO& io = ImGui::GetIO();
    const Rational fps = seq.frameRate;
    const Time mouseT = xToTime(io.MousePos.x);
    Time delta = (mouseT - drag_.startTime).snappedToFrame(fps);
    snapLine_.reset();
    pendingOp_ = nullptr;
    previewError_.clear();
    const bool snap = app.snapping != io.KeyAlt;  // Alt temporarily inverts snapping

    switch (drag_.kind) {
    case DragKind::Marquee: {
        const ImVec2 a(std::min(io.MousePos.x, drag_.startMouse.x), std::min(io.MousePos.y, drag_.startMouse.y));
        const ImVec2 b(std::max(io.MousePos.x, drag_.startMouse.x), std::max(io.MousePos.y, drag_.startMouse.y));
        std::set<ClipId> sel = drag_.marqueeBase;
        const Time ta = xToTime(a.x), tb = xToTime(b.x);
        for (const Lane& l : lanes_) {
            if (l.y + l.h < a.y || l.y > b.y) continue;
            const Track& t = *seq.tracks[static_cast<size_t>(l.track)];
            if (t.locked) continue;
            for (size_t i = t.lowerBound(ta); i < t.clips.size() && t.clips[i]->start <= tb; ++i) sel.insert(t.clips[i]->id);
        }
        if (app.linkedSelection) sel = edit::expandSelection(seq, sel, true, true);
        app.selection.clips = sel;
        return;
    }
    case DragKind::Move: {
        if (drag_.clips.empty()) return;
        if (snap) {
            auto r = snapMove(drag_.snaps, drag_.range, delta, snapThreshold());
            if (r.snapped) {
                delta = r.delta;
                snapLine_ = r.point.time;
            }
        }
        int vDelta = 0, aDelta = 0;
        const int li = laneAt(io.MousePos.y);
        if (li >= 0 && drag_.lane >= 0) {
            const Lane& from = lanes_[static_cast<size_t>(drag_.lane)];
            const Lane& to = lanes_[static_cast<size_t>(li)];
            if (from.audio == to.audio) {
                const int d = familyPos(seq, to.track) - familyPos(seq, from.track);
                (from.audio ? aDelta : vDelta) = d;
            }
        }
        const std::set<ClipId> ids = drag_.clips;
        const edit::PlaceMode mode = io.KeyCtrl ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite;
        pendingOp_ = [ids, delta, vDelta, aDelta, mode](SequenceEditor& e) { return edit::moveClips(e, ids, delta, vDelta, aDelta, mode); };
        pendingLabel_ = mode == edit::PlaceMode::Insert ? "Move (Insert)" : "Move";
        break;
    }
    case DragKind::TrimHead:
    case DragKind::TrimTail: {
        const Clip* c = app.sequence()->clip(drag_.clip);
        if (!c) return;
        const bool head = drag_.kind == DragKind::TrimHead;
        Time target = ((head ? c->start : c->end()) + delta);
        if (snap) {
            auto r = snapTime(drag_.snaps, target, snapThreshold());
            if (r.snapped) {
                target = r.time;
                snapLine_ = r.time;
            }
        }
        const ClipId id = drag_.clip;
        const bool ripple = drag_.ripple;
        const bool linked = app.linkedSelection;
        pendingOp_ = [id, head, target, ripple, linked](SequenceEditor& e) {
            return edit::trimClip(e, id, head ? edit::Edge::Head : edit::Edge::Tail, target, ripple, linked);
        };
        pendingLabel_ = ripple ? "Ripple Trim" : "Trim";
        break;
    }
    case DragKind::Roll: {
        const Clip* left = app.sequence()->clip(drag_.clip);
        if (!left) return;
        Time cut = left->end() + delta;
        if (snap) {
            auto pts = collectSnapPoints(seq, app.playhead(), {drag_.clip, drag_.other});
            auto r = snapTime(pts, cut, snapThreshold());
            if (r.snapped) {
                cut = r.time;
                snapLine_ = r.time;
            }
        }
        const ClipId l = drag_.clip, r = drag_.other;
        pendingOp_ = [l, r, cut](SequenceEditor& e) { return edit::rollEdit(e, l, r, cut); };
        pendingLabel_ = "Roll Edit";
        break;
    }
    case DragKind::Slip: {
        const ClipId id = drag_.clip;
        const bool linked = app.linkedSelection;
        const Time src = -delta;
        pendingOp_ = [id, src, linked](SequenceEditor& e) { return edit::slipClip(e, id, src, linked); };
        pendingLabel_ = "Slip";
        break;
    }
    case DragKind::Slide: {
        const Clip* c = app.sequence()->clip(drag_.clip);
        if (!c) return;
        if (snap) {
            auto r = snapMove(drag_.snaps, c->range(), delta, snapThreshold());
            if (r.snapped) {
                delta = r.delta;
                snapLine_ = r.point.time;
            }
        }
        const ClipId id = drag_.clip;
        pendingOp_ = [id, delta](SequenceEditor& e) { return edit::slideClip(e, id, delta); };
        pendingLabel_ = "Slide";
        break;
    }
    default: return;
    }

    // Live preview on a scratch copy.
    preview_.reset();
    if (pendingOp_) {
        ProjectEditor pe(app.doc().current());
        SequenceEditor se = pe.sequence(app.sequenceId());
        const Status st = pendingOp_(se);
        if (st) {
            ProjectPtr p = pe.finish();
            if (validateProject(*p).empty()) preview_ = p;
            else previewError_ = tr("Invalid result");
        } else {
            previewError_ = st.message();
        }
    }
    // Feedback tooltip
    ImGui::BeginTooltip();
    if (!previewError_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", previewError_.c_str());
    } else if (drag_.kind == DragKind::Move || drag_.kind == DragKind::Slide) {
        ImGui::Text("%s%s", delta.ticks >= 0 ? "+" : "-", formatTime(Time{delta.ticks >= 0 ? delta.ticks : -delta.ticks}, fps).c_str());
    } else if (preview_) {
        if (const Sequence* ps = preview_->findSequence(app.sequenceId()))
            if (const Clip* c = ps->clip(drag_.kind == DragKind::Roll ? drag_.clip : drag_.clip)) {
                ImGui::Text("%s %s", tr("Duration"), formatTime(c->duration, fps).c_str());
                ImGui::Text("%s %s", tr("Source In"), formatTime(c->sourceIn, fps).c_str());
            }
    }
    ImGui::EndTooltip();
}

void TimelinePanel::finishDrag(App& app) {
    const ImGuiIO& io = ImGui::GetIO();
    if (drag_.moved && pendingOp_ && previewError_.empty()) {
        app.editSequence(pendingLabel_, pendingOp_);
    } else if (!drag_.moved) {
        if (drag_.kind == DragKind::Marquee) {
            if (!io.KeyCtrl && !io.KeyShift) app.selection.clips.clear();
            app.selection.marker = kInvalidId;
        } else if (drag_.clip != kInvalidId && drag_.wasSelected && !io.KeyCtrl && !io.KeyShift) {
            app.selectClip(drag_.clip, false, false);  // click on a selected clip selects only it
        }
    }
    drag_ = Drag{};
    preview_.reset();
    pendingOp_ = nullptr;
    previewError_.clear();
    snapLine_.reset();
}

void TimelinePanel::handleDrops(App& app, const Sequence& seq) {
    dropHighlight_ = kInvalidId;
    // The lanes invisible button is the last item.
    if (!ImGui::BeginDragDropTarget()) {
        if (drag_.kind == DragKind::None && preview_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) preview_.reset();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadMedia, flags)) {
        const MediaId mid = *static_cast<const MediaId*>(p->Data);
        if (const MediaItem* m = app.project().findMedia(mid)) {
            auto target = dropTargetAt(app, io.MousePos);
            if (target) {
                Time at = target->time;
                if (app.snapping) {
                    auto pts = collectSnapPoints(seq, app.playhead(), {});
                    auto r = snapTime(pts, at, snapThreshold());
                    if (r.snapped) {
                        at = r.time;
                        snapLine_ = r.time;
                    }
                }
                const bool insert = io.KeyCtrl;
                const MediaItem media = *m;
                const int vt = target->videoTrack, at2 = target->audioTrack;
                if (p->IsDelivery()) {
                    preview_.reset();
                    snapLine_.reset();
                    app.addMediaToTimeline(mid, at, vt, at2, insert);
                } else {
                    ProjectEditor pe(app.doc().current());
                    SequenceEditor se = pe.sequence(app.sequenceId());
                    auto r = edit::addMediaClip(se, media, at, media.info.hasVideo() ? vt : -1, media.info.hasAudio() ? at2 : -1,
                                                insert ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite);
                    preview_ = r ? pe.finish() : nullptr;
                }
            }
        }
    } else if (const ImGuiPayload* p2 = ImGui::AcceptDragDropPayload(kPayloadEffect, flags)) {
        const std::string effectId(static_cast<const char*>(p2->Data));
        const Hit hit = hitTest(seq, io.MousePos);
        if (hit.clip) {
            dropHighlight_ = hit.clip->id;
            if (p2->IsDelivery()) app.applyEffectToClip(hit.clip->id, effectId);
        }
    } else if (const ImGuiPayload* p3 = ImGui::AcceptDragDropPayload(kPayloadTransition, flags)) {
        const std::string type(static_cast<const char*>(p3->Data));
        const Hit hit = hitTest(seq, io.MousePos);
        if (hit.clip) {
            dropHighlight_ = hit.clip->id;
            if (p3->IsDelivery()) {
                const ClipId id = hit.clip->id;
                const bool head = io.MousePos.x < (timeToX(hit.clip->start) + timeToX(hit.clip->end())) * 0.5f;
                const fx::TransitionDef* def = fx::findTransition(type);
                const Time dur = minTime(Time::fromSeconds(def ? def->defaultSeconds : 1.0), Time{hit.clip->duration.ticks / 2});
                app.editSequence("Add Transition", [&](SequenceEditor& e) {
                    Clip& c = e.mutableClip(id);
                    TransitionSpec s;
                    s.type = type;
                    s.duration = dur;
                    (head ? c.transitionIn : c.transitionOut) = s;
                    return Status::ok();
                });
            }
        }
    }
    ImGui::EndDragDropTarget();
}

// ------------------------------------------------------------------ menus

void TimelinePanel::clipContextMenu(App& app) {
    if (!ImGui::BeginPopup("##clipctx")) return;
    CommandRegistry& cmd = app.commands();
    auto item = [&](const char* id) {
        const Action* a = cmd.find(id);
        if (!a) return;
        if (ImGui::MenuItem(tr(a->label), cmd.shortcutText(id).c_str(), false, cmd.isEnabled(id))) cmd.run(id);
    };
    item("edit.cut");
    item("edit.copy");
    item("edit.paste");
    item("edit.duplicate");
    ImGui::Separator();
    item("edit.delete");
    item("edit.rippleDelete");
    item("timeline.split");
    ImGui::Separator();
    item("timeline.enable");
    item("timeline.link");
    item("timeline.unlink");
    item("timeline.group");
    item("timeline.ungroup");
    ImGui::Separator();
    item("timeline.speed");
    item("timeline.reverse");
    item("timeline.freeze");
    item("timeline.crossDissolve");
    item("timeline.compound");
    if (const Sequence* s = app.sequence())
        if (const Clip* c = s->clip(contextClip_); c && c->kind == ClipKind::Media) {
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Reveal in Media"))) {
                app.selection.media = {c->media};
                app.ui().showMedia = true;
            }
            if (c->transitionIn && ImGui::MenuItem(tr("Remove Transition In"))) {
                const ClipId id = c->id;
                app.editSequence("Remove Transition", [&](SequenceEditor& e) {
                    e.mutableClip(id).transitionIn.reset();
                    return Status::ok();
                });
            }
            if (c->transitionOut && ImGui::MenuItem(tr("Remove Transition Out"))) {
                const ClipId id = c->id;
                app.editSequence("Remove Transition", [&](SequenceEditor& e) {
                    e.mutableClip(id).transitionOut.reset();
                    return Status::ok();
                });
            }
        }
    ImGui::EndPopup();
}

void TimelinePanel::emptyContextMenu(App& app) {
    if (!ImGui::BeginPopup("##emptyctx")) return;
    if (ImGui::MenuItem(tr("Paste"), app.commands().shortcutText("edit.paste").c_str(), false, app.canPaste())) {
        app.seek(contextTime_);
        app.paste(false);
    }
    if (ImGui::MenuItem(tr("Close Gap")) && contextTrack_ >= 0) {
        const int track = contextTrack_;
        const Time t = contextTime_;
        app.editSequence("Close Gap", [&](SequenceEditor& e) { return edit::closeGap(e, track, t); });
    }
    if (ImGui::MenuItem(tr("Add Marker"))) {
        app.seek(contextTime_);
        app.addMarker();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Add Title"))) {
        app.seek(contextTime_);
        app.addTextClip(tr("Title"));
    }
    if (ImGui::MenuItem(tr("Add Adjustment Layer"))) {
        app.seek(contextTime_);
        app.addAdjustmentLayer();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Add Video Track"))) app.addTrack(TrackKind::Video);
    if (ImGui::MenuItem(tr("Add Audio Track"))) app.addTrack(TrackKind::Audio);
    ImGui::EndPopup();
}

}  // namespace avc::ui
