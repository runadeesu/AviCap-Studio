#include "ui/main_window.h"

#include <algorithm>
#include <cstring>

#include <imgui_internal.h>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "ui/style.h"
#include "ui/widgets.h"

#include "avicap_build_info.h"

namespace avc::ui {

namespace {

// Stable window ids independent of the UI language.
std::string winName(const char* english, const char* id) { return std::string(tr(english)) + "###" + id; }

}  // namespace

MainWindow::MainWindow(App& app) : app_(app) {
    std::snprintf(newName_, sizeof newName_, "%s", tr("Untitled"));
    setTooltipsEnabled(app.settings().ui.showTooltips);
    setTimeStyle(app.settings().ui.timecodeStyle);
}

void MainWindow::frame() {
    app_.scopesVisible = app_.ui().showScopes;
    if (!app_.ui().capturingShortcut) app_.commands().processKeyboard();
    if (app_.ui().fullscreenViewer) {
        fullscreenViewer();
    } else {
        dockspace();
        panelWindows();
    }
    settings_.draw(app_);
    shortcutSheet();
    modals();
    commandPalette();
    notifications();
}

// ------------------------------------------------------------------ layout

void MainWindow::dockspace() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float statusH = ImGui::GetFontSize() + 12.0f;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, 12));  // header height
    // The host shows the app background: dock splitters use it, so panels
    // appear as separate rounded cards with gaps between them.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal::kAppBg);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##AviCapRoot", nullptr, flags);
    ImGui::PopStyleVar(3);
    header();
    ImGui::PopStyleVar();  // FramePadding
    UiState& ui = app_.ui();
    const ImGuiID dockId = ImGui::GetID("AviCapDock");
    if (!layoutBuilt_ || ui.resetLayout || !ui.applyWorkspace.empty()) {
        // Respect a saved layout unless the user asked for a reset or another workspace.
        const bool force = ui.resetLayout || !ui.applyWorkspace.empty();
        if (force || !ImGui::DockBuilderGetNode(dockId) || ImGui::DockBuilderGetNode(dockId)->IsLeafNode())
            buildDefaultLayout(dockId, ui.applyWorkspace.empty() ? ui.workspace : ui.applyWorkspace);
        layoutBuilt_ = true;
        ui.resetLayout = false;
        ui.applyWorkspace.clear();
    }
    // Outer margin around all panels.
    const float gap = pal::kPanelGap;
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(gap, 0));
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::DockSpace(dockId, ImVec2(avail.x - gap, avail.y), ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::PopStyleColor();
    ImGui::End();
    statusBar();
}

void MainWindow::header() {
    if (!ImGui::BeginMenuBar()) return;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImRect bar = win->MenuBarRect();
    const float barH = bar.GetHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();

    // Logo: rounded gradient tile with a play glyph.
    const float logo = std::round(barH * 0.58f);
    const ImVec2 lp(bar.Min.x + 14, bar.Min.y + std::round((barH - logo) * 0.5f));
    gradientRect(dl, lp, lp + ImVec2(logo, logo), pal::kAccentHover, pal::kAccent2, logo * 0.28f);
    drawIcon(dl, Icon::Play, lp + ImVec2(logo * 0.54f, logo * 0.5f), logo * 0.62f, IM_COL32(255, 255, 255, 255));
    ImGui::SetCursorScreenPos(ImVec2(lp.x + logo + 10, ImGui::GetCursorScreenPos().y));

    menus();

    // Centre: project name with a saved / unsaved indicator.
    const bool dirty = app_.doc().dirty();
    const std::string name = app_.project().name;
    const char* state = dirty ? tr("Unsaved changes") : tr("Saved");
    ImFont* bold = boldFont() ? boldFont() : ImGui::GetFont();
    const ImVec2 nameSize = bold->CalcTextSizeA(fs, FLT_MAX, 0.0f, name.c_str());
    const ImVec2 stateSize = ImGui::CalcTextSize(state);
    const float dot = fs * 0.5f;
    const float centreW = nameSize.x + 14 + dot + 6 + stateSize.x;
    const float cx = bar.Min.x + (bar.GetWidth() - centreW) * 0.5f;
    const float menuEnd = ImGui::GetCursorScreenPos().x;
    if (cx > menuEnd + 20) {
        const float ty = bar.Min.y + (barH - fs) * 0.5f;
        dl->AddText(bold, fs, ImVec2(cx, ty), pal::kText, name.c_str());
        const float dx = cx + nameSize.x + 14;
        dl->AddCircleFilled(ImVec2(dx + dot * 0.5f, ty + fs * 0.55f), dot * 0.42f, dirty ? pal::kWarning : pal::kSuccess);
        dl->AddText(ImVec2(dx + dot + 6, ty), pal::kTextMuted, state);
    }

    // Right: workspace switcher and the export call-to-action.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 6));
    const float ctrlH = ImGui::GetFrameHeight();
    const char* wsLabels[] = {tr("Edit"), tr("Color"), tr("Audio")};
    const char* wsIds[] = {"edit", "color", "audio"};
    const char* wsTips[] = {tr("Workspace: Edit (Alt+1)"), tr("Workspace: Color (Alt+2)"), tr("Workspace: Audio (Alt+3)")};
    float segW = 6;
    for (const char* l : wsLabels) segW += ImGui::CalcTextSize(l).x + ImGui::GetStyle().FramePadding.x * 2.2f;
    const char* exportLabel = tr("Export");
    const float exportW = ImGui::CalcTextSize(exportLabel).x + ctrlH + 24;
    const float rightX = bar.Max.x - 14 - exportW - 12 - segW;
    if (rightX > cx + centreW + 16) {
        const float y = bar.Min.y + (barH - ctrlH) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(rightX, y));
        int ws = 0;
        for (int i = 0; i < 3; ++i)
            if (app_.ui().workspace == wsIds[i]) ws = i;
        if (app_.ui().workspace == "export") ws = -1;
        int sel = ws;
        if (segmented("##ws", wsLabels, 3, &sel, wsTips) || (ws < 0 && sel >= 0 && sel != ws)) app_.setWorkspace(wsIds[sel]);
        ImGui::SetCursorScreenPos(ImVec2(bar.Max.x - 14 - exportW, y));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        if (primaryButton("##exportcta", ImVec2(exportW, ctrlH))) app_.setWorkspace("export");
        tooltip(tr("Export the video (Ctrl+M)"));
        // Icon + label drawn over the button.
        drawIcon(dl, Icon::Export, bp + ImVec2(ctrlH * 0.55f, ctrlH * 0.5f), ctrlH * 0.42f, IM_COL32(255, 255, 255, 255));
        dl->AddText(bp + ImVec2(ctrlH * 0.95f, (ctrlH - fs) * 0.5f), IM_COL32(255, 255, 255, 255), exportLabel);
        if (app_.ui().workspace == "export")
            dl->AddRect(bp - ImVec2(2, 2), bp + ImVec2(exportW + 2, ctrlH + 2), pal::withAlpha(pal::kAccentHover, 0.7f), pal::kControlRadius + 2, 1.5f);
    }
    ImGui::PopStyleVar();
    ImGui::EndMenuBar();
}

void MainWindow::buildDefaultLayout(ImGuiID dockId, const std::string& workspace) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    UiState& ui = app_.ui();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);
    ImGuiID top, bottom, left, center, right, timeline, mixer;
    auto dock = [](const char* id, ImGuiID node) { ImGui::DockBuilderDockWindow(id, node); };
    // Rarely used panels share the right column as tabs behind the main one.
    auto dockExtras = [&](ImGuiID node) {
        for (const char* id : {"###History", "###Markers", "###Diagnostics"}) dock(id, node);
    };
    if (workspace == "color") {
        ui.showColor = ui.showScopes = ui.showViewer = ui.showTimeline = true;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.30f, &bottom, &top);
        ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.26f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.38f, &right, &center);
        dock("###Scopes", left);
        dock("###Library", left);
        dock("###Viewer", center);
        dock("###Color", right);
        dock("###Inspector", right);
        dock("###Export", right);
        dockExtras(right);
        dock("###Timeline", bottom);
        dock("###Mixer", bottom);
    } else if (workspace == "audio") {
        ui.showMixer = ui.showTimeline = ui.showInspector = ui.showLibrary = true;
        ui.libraryPage = "audio";
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.50f, &bottom, &top);
        ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.30f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.42f, &right, &center);
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.30f, &mixer, &timeline);
        dock("###Library", left);
        dock("###Viewer", center);
        dock("###Inspector", right);
        dock("###Scopes", right);
        dock("###Color", right);
        dock("###Export", right);
        dockExtras(right);
        dock("###Timeline", timeline);
        dock("###Mixer", mixer);
    } else if (workspace == "export") {
        ui.showExport = ui.showViewer = ui.showTimeline = true;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.28f, &bottom, &top);
        ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.42f, &right, &center);
        dock("###Viewer", center);
        dock("###Library", center);
        dock("###Scopes", center);
        dock("###Export", right);
        dock("###Inspector", right);
        dock("###Color", right);
        dockExtras(right);
        dock("###Timeline", bottom);
        dock("###Mixer", bottom);
    } else {
        ui.showLibrary = ui.showViewer = ui.showTimeline = ui.showInspector = true;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.42f, &bottom, &top);
        ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.27f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.33f, &right, &center);
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.11f, &mixer, &timeline);
        dock("###Library", left);
        dock("###Scopes", left);
        dock("###Viewer", center);
        dock("###Inspector", right);
        dock("###Color", right);
        dock("###Export", right);
        dockExtras(right);
        dock("###Timeline", timeline);
        dock("###Mixer", mixer);
    }
    ImGui::DockBuilderFinish(dockId);
    // The main panel of each area becomes its visible tab.
    focusAfterLayout_ = {"###Library", "###Inspector", "###Viewer", "###Timeline"};
    if (workspace == "color") focusAfterLayout_ = {"###Scopes", "###Color", "###Viewer", "###Timeline"};
    if (workspace == "export") focusAfterLayout_ = {"###Export", "###Viewer", "###Timeline"};
    // Docked tabs follow the focused window, one per frame: focus each main
    // panel in turn (after one frame for the docking to settle).
    focusFrames_ = 1;
}

namespace {

// Docked windows have square corners; mask them with the app background so
// every panel reads as a rounded card.
void roundPanelCorners() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w->DockIsActive) return;
    ImDrawList* dl = w->DrawList;
    const float r = pal::kPanelRadius;
    const ImVec2 a = w->Pos, b = w->Pos + w->Size;
    dl->PushClipRect(a, b, false);
    const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b};
    const ImVec2 centers[4] = {a + ImVec2(r, r), ImVec2(b.x - r, a.y + r), ImVec2(a.x + r, b.y - r), b - ImVec2(r, r)};
    const float starts[4] = {IM_PI, IM_PI * 1.5f, IM_PI * 0.5f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        dl->PathLineTo(corners[i]);
        dl->PathArcTo(centers[i], r, starts[i], starts[i] + IM_PI * 0.5f, 8);
        dl->PathFillConcave(pal::kAppBg);
    }
    dl->PopClipRect();
}

}  // namespace

void MainWindow::panelWindows() {
    UiState& ui = app_.ui();
    std::string focusNow;
    if (focusFrames_ > 0) --focusFrames_;
    else if (!focusAfterLayout_.empty()) {
        focusNow = focusAfterLayout_.front();
        focusAfterLayout_.erase(focusAfterLayout_.begin());
    }
    auto panel = [&](bool& show, const char* english, const char* id, auto&& body, ImGuiWindowFlags flags = 0) {
        if (!show) return;
        const std::string name = winName(english, id);
        const std::string key = std::string("###") + id;
        if (key == focusNow) ImGui::SetNextWindowFocus();
        if (ImGui::Begin(name.c_str(), &show, flags)) body();
        roundPanelCorners();
        ImGui::End();
    };
    panel(ui.showLibrary, "Library", "Library", [&] { library_.draw(app_); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    panel(ui.showScopes, "Scopes", "Scopes", [&] { scopes_.draw(app_); });
    panel(ui.showViewer, "Viewer", "Viewer", [&] { viewer_.draw(app_); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    panel(ui.showInspector, "Inspector", "Inspector", [&] { inspector_.draw(app_); });
    panel(ui.showColor, "Color", "Color", [&] { color_.draw(app_); });
    panel(ui.showExport, "Export", "Export", [&] { export_.draw(app_); });
    panel(ui.showHistory, "History", "History", [&] { history_.draw(app_); });
    panel(ui.showMarkers, "Markers", "Markers", [&] { markers_.draw(app_); });
    panel(ui.showDiagnostics, "Diagnostics", "Diagnostics", [&] { diagnostics_.draw(app_); });
    panel(ui.showMixer, "Mixer", "Mixer", [&] { mixer_.draw(app_); });
    panel(ui.showTimeline, "Timeline", "Timeline", [&] { timeline_.draw(app_); },
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

// ------------------------------------------------------------------ menus

void MainWindow::menuItem(const char* id, bool checked) {
    CommandRegistry& reg = app_.commands();
    const Action* a = reg.find(id);
    if (!a) return;
    if (ImGui::MenuItem(tr(a->label), reg.shortcutText(id).c_str(), checked, reg.isEnabled(id))) reg.run(id);
}

void MainWindow::menus() {
    UiState& ui = app_.ui();
    if (ImGui::BeginMenu(tr("File"))) {
        menuItem("file.new");
        menuItem("file.open");
        if (ImGui::BeginMenu(tr("Open Recent"), !app_.settings().general.recentProjects.empty())) {
            for (const auto& p : app_.settings().general.recentProjects)
                if (ImGui::MenuItem(p.c_str())) {
                    const std::string path = p;
                    app_.guardUnsaved([this, path] { app_.openProject(path); });
                }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Clear Recent"))) {
                app_.settings().general.recentProjects.clear();
                app_.saveSettings();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menuItem("file.save");
        menuItem("file.saveAs");
        ImGui::Separator();
        menuItem("file.import");
        menuItem("file.importFolder");
        menuItem("file.addMusic");
        menuItem("file.addSfx");
        menuItem("file.importSubtitles");
        menuItem("file.exportSubtitles");
        ImGui::Separator();
        menuItem("file.export");
        ImGui::Separator();
        menuItem("file.exit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Edit"))) {
        const std::string undo = std::string(tr("Undo")) + (app_.doc().canUndo() ? " " + std::string(tr(app_.doc().undoLabel().c_str())) : "");
        const std::string redo = std::string(tr("Redo")) + (app_.doc().canRedo() ? " " + std::string(tr(app_.doc().redoLabel().c_str())) : "");
        if (ImGui::MenuItem(undo.c_str(), app_.commands().shortcutText("edit.undo").c_str(), false, app_.doc().canUndo())) app_.undo();
        if (ImGui::MenuItem(redo.c_str(), app_.commands().shortcutText("edit.redo").c_str(), false, app_.doc().canRedo())) app_.redo();
        ImGui::Separator();
        for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.pasteInsert", "edit.duplicate", "edit.delete", "edit.rippleDelete"})
            menuItem(id);
        ImGui::Separator();
        menuItem("edit.selectAll");
        menuItem("edit.deselect");
        ImGui::Separator();
        menuItem("edit.preferences");
        menuItem("edit.shortcuts");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Clip"))) {
        for (const char* id : {"timeline.split", "timeline.splitAll", "timeline.enable", "timeline.link", "timeline.unlink", "timeline.group",
                               "timeline.ungroup", "timeline.speed", "timeline.reverse", "timeline.freeze", "timeline.compound",
                               "timeline.crossDissolve", "timeline.nudgeLeft", "timeline.nudgeRight"})
            menuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Sequence"))) {
        for (const char* id : {"timeline.addTitle", "timeline.addSubtitle", "timeline.addAdjustment", "timeline.addSolid"}) menuItem(id);
        ImGui::Separator();
        menuItem("timeline.addVideoTrack");
        menuItem("timeline.addAudioTrack");
        ImGui::Separator();
        for (const char* id : {"timeline.markIn", "timeline.markOut", "timeline.clearInOut", "timeline.marker", "timeline.closeGap"}) menuItem(id);
        ImGui::Separator();
        menuItem("timeline.snapping", app_.snapping);
        menuItem("timeline.linked", app_.linkedSelection);
        ImGui::Separator();
        for (const char* id : {"timeline.zoomIn", "timeline.zoomOut", "timeline.zoomFit"}) menuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Playback"))) {
        for (const char* id : {"playback.toggle", "playback.stop", "playback.forward", "playback.backward", "playback.prevFrame",
                               "playback.nextFrame", "playback.start", "playback.end", "playback.prevEdit", "playback.nextEdit"})
            menuItem(id);
        ImGui::Separator();
        menuItem("playback.loop", app_.loopPlayback);
        menuItem("playback.fullscreen", ui.fullscreenViewer);
        if (ImGui::BeginMenu(tr("Preview Quality"))) {
            const std::pair<PreviewQuality, const char*> qs[] = {{PreviewQuality::Auto, "Auto"}, {PreviewQuality::Full, "Full"},
                                                                 {PreviewQuality::Half, "1/2"}, {PreviewQuality::Quarter, "1/4"},
                                                                 {PreviewQuality::Eighth, "1/8"}};
            for (const auto& [q, n] : qs)
                if (ImGui::MenuItem(tr(n), nullptr, app_.previewQuality == q)) {
                    app_.previewQuality = q;
                    app_.settings().playback.previewQuality = previewQualityName(q);
                }
            ImGui::EndMenu();
        }
        bool proxies = app_.settings().proxy.useProxies;
        if (ImGui::MenuItem(tr("Use Proxies"), nullptr, &proxies)) {
            app_.settings().proxy.useProxies = proxies;
            app_.preview().frames().setUseProxies(proxies);
            app_.preview().frames().closeAllDecoders();
            app_.preview().invalidate();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("View"))) {
        const std::pair<const char*, bool*> views[] = {
            {"view.library", &ui.showLibrary}, {"view.viewer", &ui.showViewer},   {"view.timeline", &ui.showTimeline},
            {"view.inspector", &ui.showInspector}, {"view.color", &ui.showColor},  {"view.mixer", &ui.showMixer},
            {"view.scopes", &ui.showScopes},   {"view.export", &ui.showExport},   {"view.markers", &ui.showMarkers},
            {"view.history", &ui.showHistory}, {"view.diagnostics", &ui.showDiagnostics}};
        for (const auto& [id, flag] : views) menuItem(id, *flag);
        ImGui::Separator();
        for (const char* id : {"view.media", "view.sounds", "view.text", "view.effects", "view.transitions", "view.ai"}) {
            static const std::pair<const char*, const char*> pages[] = {{"view.media", "media"}, {"view.sounds", "audio"},
                                                                        {"view.text", "text"},   {"view.effects", "effects"},
                                                                        {"view.transitions", "transitions"}, {"view.ai", "ai"}};
            bool on = false;
            for (const auto& [cmd, page] : pages)
                if (std::strcmp(cmd, id) == 0) on = ui.showLibrary && ui.libraryPage == page;
            menuItem(id, on);
        }
        ImGui::Separator();
        for (const char* id : {"workspace.edit", "workspace.color", "workspace.audio", "workspace.export"}) {
            const std::string ws = std::string(id).substr(10);
            menuItem(id, ui.workspace == ws);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(tr("Accent color"))) {
            if (accentSwatches("##accentmenu", app_.settings().ui.accent)) app_.saveSettings();
            ImGui::EndMenu();
        }
        menuItem("view.commandPalette");
        menuItem("view.resetLayout");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Help"))) {
        menuItem("help.shortcutSheet", ui.showShortcutSheet);
        menuItem("help.about");
        menuItem("help.logs");
        menuItem("help.dataFolder");
        ImGui::EndMenu();
    }
}

// ------------------------------------------------------------------ status bar

void MainWindow::statusBar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFontSize() + 12.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal::kAppBg);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float ty = wp.y + (h - ImGui::GetFontSize()) * 0.5f - 1;
    std::string left = app_.statusText();
    if (left.empty()) {
        // Context help for the active tool, so every gesture is discoverable.
        switch (app_.tool) {
        case Tool::Razor: left = tr("Razor: click a clip to split it. Shift+click splits all tracks. V returns to the selection tool."); break;
        case Tool::Ripple: left = tr("Ripple edit: drag a clip edge; later clips move to close or open the gap."); break;
        case Tool::Roll: left = tr("Rolling edit: drag the cut between two clips to move it without changing the total length."); break;
        case Tool::Slip: left = tr("Slip: drag a clip to change which part of the source it shows."); break;
        case Tool::Slide: left = tr("Slide: drag a clip between its neighbours; they shorten or lengthen to compensate."); break;
        default:
            left = app_.sequence() && app_.sequence()->clipCount() == 0
                       ? tr("Start by importing media (Ctrl+I) or dragging files into the window. F1 shows all shortcuts.")
                       : tr("Space: play/stop  |  Ctrl+K: split  |  drag edges to trim (Ctrl: ripple)  |  Alt: toggle snapping  |  F1: shortcuts");
            break;
        }
    }
    dl->AddText(ImVec2(wp.x + 14, ty), pal::kTextFaint, left.c_str());
    // Background work as a small pill in the middle.
    std::string mid;
    float progress = -1;
    for (const auto& j : app_.exports().jobs()) {
        const auto p = j->progress();
        if (p.state == exp::ExportState::Rendering || p.state == exp::ExportState::Finalizing || p.state == exp::ExportState::Verifying) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s %.0f%%", tr("Exporting"), p.fraction() * 100.0f);
            mid = buf;
            progress = p.fraction();
        }
    }
    const size_t jobs = Jobs::allActiveJobs().size();
    if (mid.empty() && jobs > 0) mid = std::to_string(jobs) + " " + tr("background task(s)");
    const auto f = app_.preview().latest();
    char right[200];
    std::snprintf(right, sizeof right, "%s%s%dx%d  ·  %.0f ms  ·  %s", app_.stats.lowMemory ? "! " : "",
                  app_.settings().proxy.useProxies ? (std::string(tr("Proxy")) + "  ·  ").c_str() : "", f.width, f.height, f.renderMs,
                  app_.device().info().adapter.c_str());
    const float rw = ImGui::CalcTextSize(right).x;
    dl->AddText(ImVec2(wp.x + ImGui::GetWindowWidth() - rw - 14, ty), pal::kTextFaint, right);
    if (!mid.empty()) {
        const ImVec2 ms = ImGui::CalcTextSize(mid.c_str());
        const float pw = ms.x + 28 + (progress >= 0 ? 60 : 0);
        const ImVec2 pa(wp.x + ImGui::GetWindowWidth() * 0.5f - pw * 0.5f, wp.y + 3), pb(pa.x + pw, wp.y + h - 3);
        dl->AddRectFilled(pa, pb, pal::kPanelRaised, (pb.y - pa.y) * 0.5f);
        dl->AddCircleFilled(ImVec2(pa.x + 12, (pa.y + pb.y) * 0.5f), 3.5f, pal::kAccent);
        dl->AddText(ImVec2(pa.x + 22, ty), pal::kTextMuted, mid.c_str());
        if (progress >= 0) {
            const ImVec2 ba(pb.x - 66, (pa.y + pb.y) * 0.5f - 2), bb(pb.x - 10, (pa.y + pb.y) * 0.5f + 2);
            dl->AddRectFilled(ba, bb, pal::kPanelHover, 2);
            dl->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * progress, bb.y), pal::kAccent, 2);
        }
    }
    ImGui::End();
}

// ------------------------------------------------------------------ shortcut sheet

void MainWindow::shortcutSheet() {
    UiState& ui = app_.ui();
    if (!ui.showShortcutSheet) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    const std::string title = std::string(tr("Shortcut List")) + "###shortcutsheet";
    const bool sheetVisible = ImGui::Begin(title.c_str(), &ui.showShortcutSheet, ImGuiWindowFlags_NoDocking);
    floatingWindowDecor();
    if (!sheetVisible) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 9);
    ImGui::InputTextWithHint("##sheetfilter", tr("Search"), sheetFilter_, sizeof sheetFilter_);
    ImGui::SameLine();
    if (ImGui::Button(tr("Customize..."))) {
        ui.showSettings = true;
        ui.settingsTab = "Keyboard";
    }
    CommandRegistry& reg = app_.commands();
    std::string lastCat;
    const std::string needle = sheetFilter_;
    if (ImGui::BeginTable("##sheet", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& a : reg.actions()) {
            const std::string keys = reg.shortcutText(a.id);
            if (keys.empty()) continue;
            const std::string label = tr(a.label);
            if (!needle.empty() && label.find(needle) == std::string::npos && keys.find(needle) == std::string::npos) continue;
            if (a.category != lastCat) {
                lastCat = a.category;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SeparatorText(tr(a.category.c_str()));
                ImGui::TableNextColumn();
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", keys.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ------------------------------------------------------------------ notifications

void MainWindow::notifications() {
    auto& notes = app_.notifications();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float fs = ImGui::GetFontSize();
    float y = vp->WorkPos.y + vp->WorkSize.y - (fs + 12.0f) - 14;
    uint64_t dismiss = 0;
    int shown = 0;
    for (auto it = notes.rbegin(); it != notes.rend() && shown < 2; ++it) {
        const Notification& n = *it;
        if (n.level < LogLevel::Info) continue;  // debug-level notes only go to the log
        ++shown;
        const ImU32 tone = n.level >= LogLevel::Error ? pal::kDanger : n.level == LogLevel::Warning ? pal::kWarning : pal::kAccent;
        const Icon icon = n.level >= LogLevel::Error || n.level == LogLevel::Warning ? Icon::Warning : Icon::Info;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16, y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 14, 0), ImVec2(fs * 21, FLT_MAX));
        const std::string id = "##note" + std::to_string(n.id);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(32, 34, 42, 250));
        ImGui::PushStyleColor(ImGuiCol_Border, pal::kStrokeStrong);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        ImGui::Begin(id.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(p + ImVec2(fs * 0.6f, fs * 0.62f), fs * 0.62f, pal::withAlpha(tone, 0.22f));
        drawIcon(dl, icon, p + ImVec2(fs * 0.6f, fs * 0.62f), fs * 0.8f, tone);
        ImGui::Indent(fs * 1.7f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fs * 16);
        ImGui::TextUnformatted(n.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2));
        if (n.action) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal::kAccentText);
            if (ImGui::SmallButton(n.actionLabel.c_str())) {
                n.action();
                dismiss = n.id;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 16);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, pal::kTextMuted);
        if (ImGui::SmallButton(tr("Dismiss"))) dismiss = n.id;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Unindent(fs * 1.7f);
        y -= ImGui::GetWindowHeight() + 8;
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
    if (dismiss) std::erase_if(notes, [&](const Notification& n) { return n.id == dismiss; });
}

// ------------------------------------------------------------------ modals

void MainWindow::modals() {
    unsavedDialog();
    newProjectDialog();
    recoveryDialog();
    relinkDialog();
    speedDialog();
    aboutDialog();
}

void MainWindow::unsavedDialog() {
    UiState& ui = app_.ui();
    const char* id = "###unsaved";
    if (ui.unsavedPrompt && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    const std::string title = std::string(tr("Unsaved Changes")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    floatingWindowDecor();
    ImGui::Text("%s \"%s\"?", tr("Save changes to"), app_.project().name.c_str());
    ImGui::Spacing();
    auto close = [&] {
        ui.unsavedPrompt = false;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button(tr("Save"), ImVec2(ImGui::GetFontSize() * 7, 0))) {
        auto then = std::move(ui.afterUnsavedCheck);
        ui.afterUnsavedCheck = nullptr;
        app_.saveProject([then](bool ok) {
            if (ok && then) then();
        });
        close();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Don't Save"), ImVec2(ImGui::GetFontSize() * 7, 0))) {
        auto then = std::move(ui.afterUnsavedCheck);
        ui.afterUnsavedCheck = nullptr;
        close();
        if (then) then();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(ImGui::GetFontSize() * 7, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.afterUnsavedCheck = nullptr;
        close();
    }
    ImGui::EndPopup();
}

void MainWindow::newProjectDialog() {
    UiState& ui = app_.ui();
    const char* id = "###newproject";
    if (ui.showNewProject && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
        std::snprintf(newName_, sizeof newName_, "%s", tr("Untitled"));
    }
    const std::string title = std::string(tr("New Project")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    floatingWindowDecor();
    struct Fmt {
        const char* name;
        int w, h;
    };
    static const Fmt fmts[] = {{"1920x1080 (Full HD)", 1920, 1080}, {"3840x2160 (4K UHD)", 3840, 2160}, {"1280x720 (HD)", 1280, 720},
                               {"1080x1920 (Vertical / Shorts)", 1080, 1920}, {"1080x1080 (Square)", 1080, 1080},
                               {"2560x1440 (QHD)", 2560, 1440}};
    static const std::pair<const char*, Rational> rates[] = {{"23.976", {24000, 1001}}, {"24", {24, 1}}, {"25", {25, 1}},
                                                             {"29.97", {30000, 1001}},  {"30", {30, 1}}, {"50", {50, 1}},
                                                             {"59.94", {60000, 1001}},  {"60", {60, 1}}};
    ImGui::InputText(formRow(tr("Name")), newName_, sizeof newName_);
    ImGui::Combo(formRow(tr("Format")), &newPreset_, [](void*, int i) { return tr(fmts[i].name); }, nullptr, 6);
    ImGui::Combo(formRow(tr("Frame Rate")), &newRate_, [](void*, int i) { return rates[i].first; }, nullptr, 8);
    hintText(tr("The first video you import into an empty sequence can also set the format."));
    if (ImGui::Button(tr("Create"), ImVec2(ImGui::GetFontSize() * 7, 0))) {
        ProjectSettings s;
        s.width = fmts[newPreset_].w;
        s.height = fmts[newPreset_].h;
        s.frameRate = rates[newRate_].second;
        app_.newProject(newName_, s);
        ui.showNewProject = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(ImGui::GetFontSize() * 7, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showNewProject = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::recoveryDialog() {
    UiState& ui = app_.ui();
    const char* id = "###recovery";
    if (ui.showRecovery && !app_.recoveryCandidates().empty() && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    const std::string title = std::string(tr("Recover Unsaved Work")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    floatingWindowDecor();
    ImGui::TextWrapped("%s", tr("AviCap Studio did not close normally. The following projects have unsaved changes that can be recovered."));
    ImGui::Spacing();
    auto& list = app_.recoveryCandidates();
    const RecoveryCandidate* recover = nullptr;
    const RecoveryCandidate* discard = nullptr;
    for (const auto& c : list) {
        ImGui::PushID(c.metaFile.string().c_str());
        ImGui::Separator();
        ImGui::Text("%s", c.projectName.c_str());
        ImGui::TextDisabled("%s", c.projectPath.empty() ? tr("(never saved)") : c.projectPath.c_str());
        ImGui::TextDisabled("%s %s  |  %zu %s", tr("Last change"), c.lastUpdateUtc.c_str(), c.journalEntries, tr("journaled edits"));
        if (ImGui::Button(tr("Recover"))) recover = &c;
        ImGui::SameLine();
        if (ImGui::Button(tr("Discard"))) discard = &c;
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button(tr("Decide Later")) || list.empty()) {
        ui.showRecovery = false;
        ImGui::CloseCurrentPopup();
    }
    if (recover) {
        const RecoveryCandidate c = *recover;
        ui.showRecovery = false;
        ImGui::CloseCurrentPopup();
        app_.recoverProject(c);
    } else if (discard) {
        const RecoveryCandidate c = *discard;
        app_.discardRecovery(c);
    }
    ImGui::EndPopup();
}

void MainWindow::relinkDialog() {
    UiState& ui = app_.ui();
    const char* id = "###relink";
    if (ui.showRelink && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    const std::string title = std::string(tr("Relink Media")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    floatingWindowDecor();
    ImGui::TextWrapped("%s", tr("These files could not be found. Locate one file and the others are searched next to it, or search a folder."));
    int shown = 0;
    for (const auto& m : app_.project().media) {
        if (!app_.mediaOffline(m->id)) continue;
        ++shown;
        ImGui::PushID(static_cast<int>(m->id));
        ImGui::BulletText("%s", m->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Locate..."))) {
            auto files = app_.dialogs().openFiles(tr("Locate Media"), {{m->name, "*" + pathToUtf8(pathFromUtf8(m->path).extension())},
                                                                       {tr("All files"), "*.*"}}, false);
            if (!files.empty()) app_.relinkMedia(m->id, files.front());
        }
        ImGui::TextDisabled("   %s", m->path.c_str());
        ImGui::PopID();
    }
    if (shown == 0) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "%s", tr("All media is online."));
    if (ImGui::Button(tr("Search Folder..."))) {
        if (auto dir = app_.dialogs().pickFolder(tr("Search Folder"))) app_.relinkSearchFolder(*dir);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showRelink = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::speedDialog() {
    UiState& ui = app_.ui();
    const char* id = "###speed";
    if (ui.showSpeedDialog && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
        if (const Clip* c = app_.primaryClip()) speedPct_ = static_cast<float>(c->speed.toDouble() * 100.0);
    }
    const std::string title = std::string(tr("Speed/Duration")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    floatingWindowDecor();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::InputFloat(tr("Speed (%)"), &speedPct_, 5.0f, 25.0f, "%.1f");
    speedPct_ = std::clamp(speedPct_, 1.0f, 10000.0f);
    for (float preset : {25.0f, 50.0f, 100.0f, 200.0f, 400.0f}) {
        char b[16];
        std::snprintf(b, sizeof b, "%.0f%%", preset);
        if (ImGui::SmallButton(b)) speedPct_ = preset;
        ImGui::SameLine();
    }
    ImGui::NewLine();
    checkbox(tr("Ripple edit (shift following clips)"), &speedRipple_);
    if (const Clip* c = app_.primaryClip()) {
        const Time newDur = Time{static_cast<int64_t>(static_cast<double>(c->sourceDuration().ticks) / (speedPct_ / 100.0))};
        ImGui::TextDisabled("%s: %s", tr("New duration"), formatTime(newDur, app_.sequence()->frameRate).c_str());
    }
    if (ImGui::Button(tr("OK"), ImVec2(ImGui::GetFontSize() * 6, 0))) {
        app_.setSelectionSpeed(Rational{static_cast<int64_t>(std::lround(speedPct_ * 10.0f)), 1000}.reduced(), speedRipple_);
        ui.showSpeedDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showSpeedDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::aboutDialog() {
    UiState& ui = app_.ui();
    const char* id = "###about";
    if (ui.showAbout && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    const std::string title = std::string(tr("About AviCap Studio")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    floatingWindowDecor();
    ImGui::Text("AviCap Studio %s", AVICAP_VERSION_STRING);
    ImGui::TextDisabled("%s  |  %s", AVICAP_BUILD_DATE, AVICAP_COMPILER);
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Video editor for Windows. Local first: your media never leaves this PC unless you choose a cloud feature."));
    hintText(tr("Uses FFmpeg (LGPL), Dear ImGui, nlohmann/json, stb, doctest. See THIRD_PARTY_LICENSES.md."));
    ImGui::Text("%s: %s", tr("Render device"), app_.device().info().adapter.c_str());
    if (ImGui::Button(tr("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showAbout = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::commandPalette() {
    UiState& ui = app_.ui();
    const char* id = "###palette";
    if (ui.showCommandPalette && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
        paletteQuery_[0] = 0;
        paletteIndex_ = 0;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.15f), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32, 0));
    if (!ImGui::BeginPopup(id)) {
        ui.showCommandPalette = false;
        return;
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##q", tr("Type a command"), paletteQuery_, sizeof paletteQuery_);
    std::string q = paletteQuery_;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<const Action*> hits;
    for (const auto& a : app_.commands().actions()) {
        std::string l1 = tr(a.label), l2 = a.label;
        std::transform(l2.begin(), l2.end(), l2.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (q.empty() || l1.find(paletteQuery_) != std::string::npos || l2.find(q) != std::string::npos) hits.push_back(&a);
        if (hits.size() >= 14) break;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) paletteIndex_ = std::min(paletteIndex_ + 1, static_cast<int>(hits.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) paletteIndex_ = std::max(paletteIndex_ - 1, 0);
    std::string run;
    for (int i = 0; i < static_cast<int>(hits.size()); ++i) {
        const Action* a = hits[static_cast<size_t>(i)];
        const bool enabled = app_.commands().isEnabled(a->id);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Selectable(tr(a->label), i == paletteIndex_)) run = a->id;
        ImGui::EndDisabled();
        const std::string sc = app_.commands().shortcutText(a->id);
        if (!sc.empty()) {
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(sc.c_str()).x - 16);
            ImGui::TextDisabled("%s", sc.c_str());
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && paletteIndex_ < static_cast<int>(hits.size())) run = hits[static_cast<size_t>(paletteIndex_)]->id;
    if (!run.empty() || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showCommandPalette = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (!run.empty()) app_.commands().run(run);
}

void MainWindow::fullscreenViewer() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::Begin("##fullscreen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
    viewer_.drawImage(app_, ImGui::GetContentRegionAvail(), true);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
        app_.ui().fullscreenViewer = false;
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ drops

void MainWindow::onFilesDropped(const std::vector<std::string>& paths, ImVec2 screenPos) {
    if (paths.empty()) return;
    // A dropped project file opens instead of importing.
    if (paths.size() == 1 && pathFromUtf8(paths[0]).extension() == ".avicap") {
        const std::string p = paths[0];
        app_.guardUnsaved([this, p] { app_.openProject(p); });
        return;
    }
    // Subtitle files become subtitle clips (timed from the start of the sequence).
    std::vector<std::string> media;
    for (const auto& p : paths) {
        const std::string ext = toLower(pathToUtf8(pathFromUtf8(p).extension()));
        if (ext == ".srt" || ext == ".vtt") {
            if (auto r = app_.importSubtitles(p); !r) app_.notify(LogLevel::Warning, r.errorMessage());
        } else {
            media.push_back(p);
        }
    }
    if (media.empty()) return;
    auto target = timeline_.dropTargetAt(app_, screenPos);
    if (!target) {
        app_.importFiles(media);
        return;
    }
    const auto t = *target;
    App* app = &app_;
    app_.importFiles(media, [app, t](const std::vector<MediaId>& ids) {
        Time at = t.time;
        for (MediaId id : ids) {
            auto r = app->addMediaToTimeline(id, at, t.videoTrack, t.audioTrack, false);
            if (!r || r->empty()) continue;
            if (const Clip* c = app->sequence()->clip(r->front())) at = c->end();
        }
    });
}

}  // namespace avc::ui
