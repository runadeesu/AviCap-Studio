// Program monitor: displays the preview renderer's latest frame, the
// transport bar, quality selection, safe-area guides and an on-canvas handle
// to move the selected clip.

#include <algorithm>
#include <cmath>

#include "core/i18n.h"
#include "core/strings.h"
#include "render/compositor.h"
#include "ui/edit_helpers.h"
#include "ui/panels.h"
#include "ui/style.h"
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
    const float fs = ImGui::GetFontSize();
    const float s = ImGui::GetFrameHeight();
    const float playD = s * 1.35f;  // the round play button
    const float rowH = playD;
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Left: current time (editable) and duration.
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (rowH - s) * 0.5f));
    Time t = app.playhead();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kAccentText);
    ImGui::PushFont(boldFont() ? boldFont() : ImGui::GetFont(), ImGui::GetStyle().FontSizeBase * 1.05f);
    if (timeField("##tc", t, seq->frameRate, fs * 7.2f)) app.seek(t);
    ImGui::PopFont();
    ImGui::PopStyleColor(2);
    tooltip(tr("Current time (type a timecode, frames or seconds)"));
    ImGui::SameLine(0, 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::v4(pal::kTextFaint), "/ %s", formatTime(seq->duration(), seq->frameRate).c_str());

    // Centre: transport with a large round play/pause button.
    const float small = s * 0.95f;
    const float groupW = small * 2 + 4 + playD + 8 + 4 + small * 2 + 10 + small;
    const float cx = row.x + std::max(width * 0.5f - groupW * 0.5f, ImGui::GetItemRectMax().x - row.x + 12);
    float x = cx;
    auto btn = [&](const char* id, Icon icon, const char* tip, bool active = false) {
        ImGui::SetCursorScreenPos(ImVec2(x, row.y + (rowH - small) * 0.5f));
        const bool r = iconButton(id, icon, tip, active, small);
        x += small + 2;
        return r;
    };
    if (btn("##start", Icon::ToStart, tr("Go to Start (Home)"))) app.goToStart();
    if (btn("##prev", Icon::StepBack, tr("Previous Frame (Left)"))) app.stepFrames(-1);
    x += 4;
    ImGui::SetCursorScreenPos(ImVec2(x, row.y));
    const bool playing = app.playing();
    if (ImGui::InvisibleButton("##play", ImVec2(playD, playD))) app.togglePlay();
    const bool hov = ImGui::IsItemHovered();
    tooltip(tr("Play/Pause (Space)"));
    const ImVec2 pc(x + playD * 0.5f, row.y + playD * 0.5f);
    dl->AddCircleFilled(pc, playD * 0.5f, hov ? pal::kAccentHover : pal::kAccent, 32);
    drawIcon(dl, playing ? Icon::Pause : Icon::Play, pc + ImVec2(playing ? 0.0f : playD * 0.04f, 0), playD * 0.5f, IM_COL32(255, 255, 255, 255));
    x += playD + 6;
    if (btn("##next", Icon::StepForward, tr("Next Frame (Right)"))) app.stepFrames(1);
    if (btn("##end", Icon::ToEnd, tr("Go to End (End)"))) app.goToEnd();
    x += 8;
    if (btn("##loop", Icon::Loop, tr("Loop Playback"), app.loopPlayback)) app.setLoop(!app.loopPlayback);
    if (playing && std::fabs(app.playSpeed() - 1.0) > 0.01) {
        char sp[16];
        std::snprintf(sp, sizeof sp, "%+.0fx", app.playSpeed());
        dl->AddText(ImVec2(x + 4, row.y + (rowH - fs) * 0.5f), pal::kWarning, sp);
    }

    // Right: frame shape (16:9, 9:16...), view options and full screen.
    int presetCount = 0;
    const AspectPreset* presets = aspectPresets(&presetCount);
    const char* ratioLabel = tr("Custom");
    if (seq)
        for (int i = 0; i < presetCount; ++i)
            if (presets[i].matches(seq->width, seq->height)) ratioLabel = presets[i].label;
    const float ratioW = std::max(ImGui::CalcTextSize(ratioLabel).x, ImGui::CalcTextSize("21:9").x) + fs * 2.2f;
    const float rightW = ratioW + 6 + small + 2 + small;
    float rx = row.x + width - rightW;
    if (rx > x + fs) {
        const float bh = ImGui::GetFrameHeight();
        const ImVec2 bp(rx, row.y + (rowH - bh) * 0.5f);
        ImGui::SetCursorScreenPos(bp);
        const bool ratioPressed = ImGui::InvisibleButton("##ratio", ImVec2(ratioW, bh));
        const bool ratioHover = ImGui::IsItemHovered();
        tooltip(tr("Aspect Ratio"));
        dl->AddRectFilled(bp, bp + ImVec2(ratioW, bh), ratioHover ? pal::kPanelHover : pal::kPanelRaised, bh * 0.5f);
        const ImVec2 rts = ImGui::CalcTextSize(ratioLabel);
        dl->AddText(ImVec2(bp.x + fs * 0.7f, bp.y + (bh - rts.y) * 0.5f), pal::kText, ratioLabel);
        const ImVec2 cc(bp.x + ratioW - fs * 0.8f, bp.y + bh * 0.5f);
        dl->PathLineTo(cc + ImVec2(-fs * 0.22f, -fs * 0.1f));
        dl->PathLineTo(cc + ImVec2(0, fs * 0.12f));
        dl->PathLineTo(cc + ImVec2(fs * 0.22f, -fs * 0.1f));
        dl->PathStroke(pal::kTextMuted, 1.5f);
        if (ratioPressed) ImGui::OpenPopup("##ratiomenu");
        rx += ratioW + 6;
        ImGui::SetCursorScreenPos(ImVec2(rx, row.y + (rowH - small) * 0.5f));
        if (iconButton("##more", Icon::More, tr("View options"), false, small)) ImGui::OpenPopup("##viewopts");
        rx += small + 2;
        ImGui::SetCursorScreenPos(ImVec2(rx, row.y + (rowH - small) * 0.5f));
        if (iconButton("##full", Icon::Fullscreen, tr("Full Screen Viewer (Ctrl+F)"), false, small)) app.ui().fullscreenViewer = true;
        if (ImGui::BeginPopup("##ratiomenu")) {
            ImGui::TextDisabled("%s", tr("Aspect Ratio"));
            for (int i = 0; i < presetCount; ++i) {
                const bool on = seq && presets[i].matches(seq->width, seq->height);
                char item[160];
                std::snprintf(item, sizeof item, "%-6s %s", presets[i].label, tr(presets[i].hint));
                if (ImGui::MenuItem(item, nullptr, on) && !on) app.setSequenceAspect(presets[i].num, presets[i].den);
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##viewopts")) {
            if (ImGui::BeginMenu(tr("Preview Quality"))) {
                for (PreviewQuality q : {PreviewQuality::Auto, PreviewQuality::Full, PreviewQuality::Half, PreviewQuality::Quarter,
                                         PreviewQuality::Eighth})
                    if (ImGui::MenuItem(tr(qualityLabel(q)), nullptr, q == app.previewQuality)) {
                        app.previewQuality = q;
                        app.settings().playback.previewQuality = previewQualityName(q);
                    }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(tr("Zoom"))) {
                const char* zooms[] = {tr("Fit"), "25%", "50%", "75%", "100%", "150%", "200%", "400%"};
                const int zv[] = {0, 25, 50, 75, 100, 150, 200, 400};
                for (int i = 0; i < 8; ++i)
                    if (ImGui::MenuItem(zooms[i], nullptr, zv[i] == zoom_)) zoom_ = zv[i];
                ImGui::EndMenu();
            }
            ImGui::Separator();
            bool proxies = app.settings().proxy.useProxies;
            if (ImGui::MenuItem(tr("Use Proxies"), nullptr, &proxies)) {
                app.settings().proxy.useProxies = proxies;
                app.preview().frames().setUseProxies(proxies);
                app.preview().frames().closeAllDecoders();
                app.preview().invalidate();
            }
            ImGui::MenuItem(tr("Safe area guides"), nullptr, &safeAreas_);
            ImGui::MenuItem(tr("Show render statistics"), nullptr, &showStats_);
            ImGui::EndPopup();
        }
    }
    ImGui::SetCursorScreenPos(row);
    ImGui::Dummy(ImVec2(width, rowH));
}

void ViewerPanel::drawWelcome(App& app, ImVec2 size) {
    ImGui::BeginChild("##welcome", size, ImGuiChildFlags_None);
    const float fs = ImGui::GetFontSize();
    const float colW = std::min(size.x - 32.0f, fs * 46.0f);
    const float x0 = std::max(16.0f, (size.x - colW) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* bold = boldFont() ? boldFont() : ImGui::GetFont();
    ImGui::SetCursorPos(ImVec2(x0, std::max(14.0f, size.y * 0.07f)));
    ImGui::BeginGroup();
    // Hero
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.85f);
    ImGui::TextUnformatted(tr("Welcome to AviCap Studio"));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextMuted);
    wrappedText(tr("Let's start editing. Everything stays on this PC."));
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, fs * 0.6f));

    // Action cards
    const float gap = 10.0f;
    const float ir = fs * 1.15f;
    const float bw = (colW - gap * 2) / 3.0f, bh = 16 + ir * 2 + 12 + fs * 1.1f + 6 + fs + 16;
    auto card = [&](const char* id, Icon icon, const char* label, const char* hint, bool primary) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(bw, bh));
        const bool hov = ImGui::IsItemHovered();
        if (primary) {
            gradientRect(dl, p, p + ImVec2(bw, bh), hov ? pal::kAccentHover : pal::kAccent, pal::mix(pal::kAccentPressed, pal::kAccent2, 0.5f), 12.0f);
            dl->AddRectFilled(p, p + ImVec2(bw, bh), IM_COL32(255, 255, 255, hov ? 18 : 0), 12.0f);
        } else {
            dl->AddRectFilled(p, p + ImVec2(bw, bh), hov ? pal::kPanelHover : pal::kPanelRaised, 12.0f);
            dl->AddRect(p, p + ImVec2(bw, bh), pal::kStroke, 12.0f);
        }
        const ImVec2 ic = p + ImVec2(16 + ir, 16 + ir);
        dl->AddCircleFilled(ic, ir, primary ? IM_COL32(255, 255, 255, 46) : pal::kAccentSoft, 28);
        drawIcon(dl, icon, ic, ir * 1.05f, primary ? IM_COL32(255, 255, 255, 255) : pal::kAccentText);
        const float ty = 16 + ir * 2 + 12;
        dl->PushClipRect(p, p + ImVec2(bw - 6, bh), true);
        const std::string labelShown = ellipsize(bold, fs * 1.05f, label, bw - 28);
        const std::string hintShown = ellipsize(ImGui::GetFont(), fs, hint, bw - 28);
        dl->AddText(bold, fs * 1.05f, p + ImVec2(16, ty), primary ? IM_COL32(255, 255, 255, 255) : pal::kText, labelShown.c_str());
        dl->AddText(p + ImVec2(16, ty + fs * 1.1f + 6), primary ? IM_COL32(255, 255, 255, 190) : pal::kTextMuted, hintShown.c_str());
        if (hov && (labelShown != label || hintShown != hint)) tooltip((std::string(label) + "\n" + hint).c_str());
        dl->PopClipRect();
        return pressed;
    };
    if (card("##w_import", Icon::Plus, tr("Import Media"), tr("Ctrl+I or drag files here"), true)) app.importDialog();
    ImGui::SameLine(0, gap);
    if (card("##w_new", Icon::Film, tr("New Project"), "Ctrl+N", false)) app.commands().run("file.new");
    ImGui::SameLine(0, gap);
    if (card("##w_open", Icon::Folder, tr("Open Project"), "Ctrl+O", false)) app.openProjectDialog();
    ImGui::Dummy(ImVec2(0, fs * 0.8f));

    // Editing flow as five steps with icons (details in tooltips).
    headingText(tr("How to edit"));
    ImGui::Dummy(ImVec2(0, 2));
    struct Step {
        Icon icon;
        const char* label;
        const char* detail;
    };
    const Step steps[] = {
        {Icon::Plus, trNoop("Import"), "1. Import video, music and images (or drag files into the window)."},
        {Icon::Film, trNoop("Arrange"), "2. Drag clips from the Media panel to the timeline (double-click adds at the playhead)."},
        {Icon::Razor, trNoop("Cut & Text"), "3. Cut with Ctrl+K, add text (Ctrl+T), subtitles, BGM and sound effects from the toolbar."},
        {Icon::Palette, trNoop("Look"), "4. Add effects and color from the Effects / Color panels; press Space to preview."},
        {Icon::Export, trNoop("Export"), "5. Export with Ctrl+M (presets for YouTube, TikTok, Shorts...)."},
    };
    const float arrow = fs * 1.1f;
    const float sw = (colW - arrow * 4) / 5.0f, sh = fs * 3.6f;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) {
            ImGui::SameLine(0, 0);
            const ImVec2 ap = ImGui::GetCursorScreenPos();
            drawIcon(dl, Icon::KeyNext, ap + ImVec2(arrow * 0.5f, sh * 0.5f), fs * 0.7f, pal::kTextFaint);
            ImGui::Dummy(ImVec2(arrow, sh));
            ImGui::SameLine(0, 0);
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImGui::InvisibleButton("##step", ImVec2(sw, sh));
        ImGui::PopID();
        const bool hov = ImGui::IsItemHovered();
        tooltip(tr(steps[i].detail));
        dl->AddRectFilled(p, p + ImVec2(sw, sh), hov ? pal::kPanelHover : pal::kPanelRaised, 10.0f);
        char num[4];
        std::snprintf(num, sizeof num, "%d", i + 1);
        dl->AddCircleFilled(p + ImVec2(fs * 0.95f, fs * 0.95f), fs * 0.55f, pal::kAccentSoft, 20);
        dl->AddText(bold, fs * 0.75f, p + ImVec2(fs * 0.95f - fs * 0.2f, fs * 0.95f - fs * 0.4f), pal::kAccentText, num);
        drawIcon(dl, steps[i].icon, p + ImVec2(sw * 0.5f, sh * 0.42f), fs * 1.1f, pal::kText);
        const char* lbl = tr(steps[i].label);
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(p + ImVec2((sw - ts.x) * 0.5f, sh - fs * 1.35f), pal::kTextMuted, lbl);
    }

    // Recent projects
    const auto& recent = app.settings().general.recentProjects;
    if (!recent.empty()) {
        ImGui::Dummy(ImVec2(0, fs * 0.6f));
        headingText(tr("Recent Projects"));
        ImGui::Dummy(ImVec2(0, 2));
        int shown = 0;
        for (const auto& r : recent) {
            if (++shown > 5) break;
            const std::string name = pathToUtf8(pathFromUtf8(r).stem());
            ImGui::PushID(r.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rh = fs * 2.6f;
            if (ImGui::InvisibleButton("##recent", ImVec2(colW, rh))) {
                const std::string path = r;
                app.guardUnsaved([&app, path] { app.openProject(path); });
            }
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(p, p + ImVec2(colW, rh), hov ? pal::kPanelHover : pal::kPanelRaised, 8.0f);
            drawIcon(dl, Icon::Film, p + ImVec2(fs * 1.3f, rh * 0.5f), fs * 1.0f, pal::kAccentText);
            dl->AddText(bold, fs, p + ImVec2(fs * 2.6f, rh * 0.5f - fs * 1.05f), pal::kText, name.c_str());
            dl->PushClipRect(p, p + ImVec2(colW - 8, rh), true);
            dl->AddText(ImGui::GetFont(), fs * 0.85f, p + ImVec2(fs * 2.6f, rh * 0.5f + fs * 0.05f), pal::kTextFaint, r.c_str());
            dl->PopClipRect();
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 1));
        }
    }
    ImGui::EndGroup();
    ImGui::EndChild();
}

void ViewerPanel::draw(App& app) {
    const Sequence* seqW = app.sequence();
    if (seqW && seqW->clipCount() == 0 && app.project().media.empty() && !app.busyLoading()) {
        drawWelcome(app, ImGui::GetContentRegionAvail());
        return;
    }
    const float transportH = ImGui::GetFrameHeight() * 1.35f + ImGui::GetStyle().ItemSpacing.y + 8;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(20.0f, avail.y - transportH);
    drawImage(app, avail, false);
    ImGui::Spacing();
    drawTransport(app);
}

}  // namespace avc::ui
