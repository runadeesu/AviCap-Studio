// Program monitor: displays the preview renderer's latest frame, the
// transport bar, quality selection, safe-area guides and an on-canvas handle
// to move the selected clip.

#include <algorithm>
#include <cmath>

#include "core/i18n.h"
#include "render/compositor.h"
#include "ui/edit_helpers.h"
#include "ui/panels.h"
#include "ui/widgets.h"

namespace avc::ui {

namespace {
const char* qualityLabel(PreviewQuality q) {
    switch (q) {
    case PreviewQuality::Full: return "Full";
    case PreviewQuality::Half: return "1/2";
    case PreviewQuality::Quarter: return "1/4";
    case PreviewQuality::Eighth: return "1/8";
    default: return "Auto";
    }
}
}  // namespace

bool ViewerPanel::drawImage(App& app, ImVec2 size, bool fullscreen) {
    const Sequence* seq = app.sequence();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##viewer_image", ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p0 + size, IM_COL32(12, 12, 14, 255));
    if (!seq) return false;
    app.viewerWidth = static_cast<int>(size.x);
    app.viewerHeight = static_cast<int>(size.y);

    // Fit (or zoom) the sequence frame into the area.
    const float seqW = static_cast<float>(seq->width), seqH = static_cast<float>(seq->height);
    float scale = std::min(size.x / seqW, size.y / seqH);
    if (!fullscreen && zoom_ > 0) scale = static_cast<float>(zoom_) / 100.0f;
    const ImVec2 imgSize(seqW * scale, seqH * scale);
    const ImVec2 imgMin = p0 + ImVec2((size.x - imgSize.x) * 0.5f, (size.y - imgSize.y) * 0.5f);
    const ImVec2 imgMax = imgMin + imgSize;
    dl->PushClipRect(p0, p0 + size, true);
    dl->AddRectFilled(imgMin, imgMax, IM_COL32(0, 0, 0, 255));
    const PreviewFrame f = app.preview().latest();
    if (f.texture) {
        void* view = app.device().nativeView(*f.texture);
        if (view) dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(view)), imgMin, imgMax);
        else dl->AddText(imgMin + ImVec2(8, 8), IM_COL32(150, 150, 150, 255), tr("Preview (software renderer)"));
    }
    if (f.missingFrames > 0)
        dl->AddText(imgMin + ImVec2(10, 10), IM_COL32(255, 90, 90, 255), tr("Media Offline or unreadable - see the Media panel"));
    if (!f.error.empty()) dl->AddText(imgMin + ImVec2(10, 30), IM_COL32(255, 90, 90, 255), f.error.c_str());
    if (!fullscreen) drawOverlays(app, dl, imgMin, imgMax);
    if (showStats_ || fullscreen) {
        const auto st = app.preview().stats();
        char buf[160];
        std::snprintf(buf, sizeof buf, "%dx%d (1/%d)  %.1f ms  %s", f.width, f.height, f.divisor, f.renderMs,
                      st.decoder.empty() ? "" : st.decoder.c_str());
        if (showStats_) dl->AddText(ImVec2(imgMin.x + 6, imgMax.y - ImGui::GetFontSize() - 4), IM_COL32(220, 220, 120, 255), buf);
    }
    dl->PopClipRect();

    // Transform handle: drag the selected visual clip in the frame.
    const ImGuiIO& io = ImGui::GetIO();
    const Clip* clip = app.primaryClip();
    const Track* track = clip ? seq->trackOfClip(clip->id) : nullptr;
    const bool movable = clip && track && isVisualTrack(track->kind) && !track->locked && clip->kind != ClipKind::Adjustment &&
                         clip->range().contains(app.playhead());
    if (!fullscreen && ImGui::IsItemActivated() && movable) {
        dragging_ = true;
        dragStart_ = io.MousePos;
        dragClip_ = clip->id;
        posStart_ = clip->transform.evaluate("position", clipLocalTime(*clip, app.playhead()), pv(0, 0));
    }
    if (dragging_) {
        if (ImGui::IsItemActive() && clip && clip->id == dragClip_) {
            const ImVec2 d = io.MousePos - dragStart_;
            if (std::fabs(d.x) + std::fabs(d.y) > 1.0f) {
                ParamValue v = posStart_;
                v[0] += d.x / scale;
                v[1] += d.y / scale;
                if (io.KeyShift) {  // constrain to one axis
                    if (std::fabs(d.x) > std::fabs(d.y)) v[1] = posStart_[1];
                    else v[0] = posStart_[0];
                }
                v[0] = std::round(v[0]);
                v[1] = std::round(v[1]);
                setParamValue(app, ParamRef{dragClip_, ParamTarget::Transform, kInvalidId, "position"}, v,
                              clipLocalTime(*clip, app.playhead()), "viewer-position:" + std::to_string(dragClip_));
            }
        } else {
            dragging_ = false;
        }
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0 && io.KeyCtrl && !fullscreen) {
        const int steps[] = {0, 25, 50, 75, 100, 150, 200, 400};
        int i = 0;
        for (int k = 0; k < 8; ++k)
            if (steps[k] == zoom_) i = k;
        i = std::clamp(i + (io.MouseWheel > 0 ? 1 : -1), 0, 7);
        zoom_ = steps[i];
    }
    return true;
}

void ViewerPanel::drawOverlays(App& app, ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax) {
    const ImVec2 size = imgMax - imgMin;
    if (safeAreas_) {
        for (float pct : {0.93f, 0.90f}) {
            const ImVec2 inset = size * ((1.0f - pct) * 0.5f);
            dl->AddRect(imgMin + inset, imgMax - inset, IM_COL32(255, 255, 255, 90), 0, 1.0f);
        }
        const ImVec2 c = (imgMin + imgMax) * 0.5f;
        dl->AddLine(c - ImVec2(10, 0), c + ImVec2(10, 0), IM_COL32(255, 255, 255, 90));
        dl->AddLine(c - ImVec2(0, 10), c + ImVec2(0, 10), IM_COL32(255, 255, 255, 90));
    }
    // Bounding box of the selected visual clip.
    const Sequence* seq = app.sequence();
    const Clip* clip = app.primaryClip();
    if (!seq || !clip || !clip->range().contains(app.playhead())) return;
    const Track* track = seq->trackOfClip(clip->id);
    if (!track || !isVisualTrack(track->kind)) return;
    float lw = static_cast<float>(seq->width), lh = static_cast<float>(seq->height);
    int rotation = 0;
    Rational sar{1, 1};
    if (clip->kind == ClipKind::Media) {
        const MediaItem* m = app.project().findMedia(clip->media);
        const VideoStreamInfo* v = m ? m->info.primaryVideo() : nullptr;
        if (!v) return;
        lw = static_cast<float>(v->width);
        lh = static_cast<float>(v->height);
        rotation = v->rotation;
        sar = v->sampleAspect;
    } else if (clip->kind == ClipKind::Text || clip->kind == ClipKind::Subtitle) {
        return;  // text bounds depend on the rasterizer; the inspector edits them numerically
    }
    const LayerGeometry g = computeLayerGeometry(*clip, clipLocalTime(*clip, app.playhead()), lw, lh, rotation, sar, seq->width, seq->height);
    const float sx = size.x / static_cast<float>(seq->width), sy = size.y / static_cast<float>(seq->height);
    ImVec2 pts[4];
    const float cx[4] = {g.cropX0, g.width - g.cropX1, g.width - g.cropX1, g.cropX0};
    const float cy[4] = {g.cropY0, g.cropY0, g.height - g.cropY1, g.height - g.cropY1};
    for (int i = 0; i < 4; ++i) {
        float x = cx[i], y = cy[i];
        g.toSequence.apply(x, y);
        pts[i] = ImVec2(imgMin.x + x * sx, imgMin.y + y * sy);
    }
    dl->AddPolyline(pts, 4, IM_COL32(80, 160, 255, 220), 1.5f, ImDrawFlags_Closed);
    for (const ImVec2& p : pts) dl->AddRectFilled(p - ImVec2(3, 3), p + ImVec2(3, 3), IM_COL32(80, 160, 255, 255));
    const ImVec2 c = (pts[0] + pts[2]) * 0.5f;
    dl->AddCircle(c, 5, IM_COL32(80, 160, 255, 255), 12, 1.5f);
}

void ViewerPanel::drawTransport(App& app) {
    const Sequence* seq = app.sequence();
    if (!seq) return;
    const float s = ImGui::GetFrameHeight();
    Time t = app.playhead();
    if (timeField("##tc", t, seq->frameRate, ImGui::GetFontSize() * 7.5f)) app.seek(t);
    tooltip(tr("Current time (type a timecode, frames or seconds)"));
    ImGui::SameLine(0, 8);
    const float buttons = s * 7 + 6 * 2;
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - buttons) * 0.5f - s * 4));
    if (iconButton("##start", Icon::ToStart, tr("Go to Start (Home)"), false, s)) app.goToStart();
    ImGui::SameLine(0, 2);
    if (iconButton("##prev", Icon::StepBack, tr("Previous Frame (Left)"), false, s)) app.stepFrames(-1);
    ImGui::SameLine(0, 2);
    if (iconButton("##rev", Icon::KeyPrev, tr("Play Backward (J)"), app.playing() && app.playSpeed() < 0, s)) app.shuttle(-1);
    ImGui::SameLine(0, 2);
    if (iconButton("##play", app.playing() ? Icon::Pause : Icon::Play, tr("Play/Pause (Space)"), false, s)) app.togglePlay();
    ImGui::SameLine(0, 2);
    if (iconButton("##ff", Icon::KeyNext, tr("Play Forward (L)"), app.playing() && app.playSpeed() > 1.01, s)) app.shuttle(1);
    ImGui::SameLine(0, 2);
    if (iconButton("##next", Icon::StepForward, tr("Next Frame (Right)"), false, s)) app.stepFrames(1);
    ImGui::SameLine(0, 2);
    if (iconButton("##end", Icon::ToEnd, tr("Go to End (End)"), false, s)) app.goToEnd();
    ImGui::SameLine(0, 2);
    if (iconButton("##loop", Icon::Loop, tr("Loop Playback"), app.loopPlayback, s)) app.setLoop(!app.loopPlayback);
    if (app.playing() && std::fabs(app.playSpeed() - 1.0) > 0.01) {
        ImGui::SameLine(0, 6);
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%+.0fx", app.playSpeed());
    }
    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
    if (ImGui::BeginCombo("##quality", tr(qualityLabel(app.previewQuality)))) {
        for (PreviewQuality q : {PreviewQuality::Auto, PreviewQuality::Full, PreviewQuality::Half, PreviewQuality::Quarter,
                                 PreviewQuality::Eighth})
            if (ImGui::Selectable(tr(qualityLabel(q)), q == app.previewQuality)) {
                app.previewQuality = q;
                app.settings().playback.previewQuality = previewQualityName(q);
            }
        ImGui::EndCombo();
    }
    tooltip(tr("Preview quality (Auto lowers resolution during playback when needed)"));
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    const char* zooms[] = {"Fit", "25%", "50%", "75%", "100%", "150%", "200%", "400%"};
    const int zv[] = {0, 25, 50, 75, 100, 150, 200, 400};
    int zi = 0;
    for (int i = 0; i < 8; ++i)
        if (zv[i] == zoom_) zi = i;
    if (ImGui::Combo("##zoom", &zi, zooms, 8)) zoom_ = zv[zi];
    ImGui::SameLine(0, 4);
    bool proxies = app.settings().proxy.useProxies;
    if (textToggle("P", &proxies, tr("Use proxies for playback"), IM_COL32(200, 140, 40, 255))) {
        app.settings().proxy.useProxies = proxies;
        app.preview().frames().setUseProxies(proxies);
        app.preview().frames().closeAllDecoders();
        app.preview().invalidate();
    }
    ImGui::SameLine(0, 2);
    textToggle(tr("Safe"), &safeAreas_, tr("Safe area guides"));
    ImGui::SameLine(0, 2);
    textToggle("i", &showStats_, tr("Show render statistics"));
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("/ %s", formatTime(seq->duration(), seq->frameRate).c_str());
}

void ViewerPanel::draw(App& app) {
    const float transportH = ImGui::GetFrameHeightWithSpacing() + 6;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(20.0f, avail.y - transportH);
    drawImage(app, avail, false);
    ImGui::Spacing();
    drawTransport(app);
}

}  // namespace avc::ui
