#include "ui/main_window.h"

#include <algorithm>
#include <cstring>

#include <imgui_internal.h>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
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
    modals();
    commandPalette();
    notifications();
}

// ------------------------------------------------------------------ layout

void MainWindow::dockspace() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float statusH = ImGui::GetFrameHeight() + 4;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##AviCapRoot", nullptr, flags);
    ImGui::PopStyleVar(3);
    menuBar();
    const ImGuiID dockId = ImGui::GetID("AviCapDock");
    if (!layoutBuilt_ || app_.ui().resetLayout) {
        // Respect a saved layout unless the user asked for a reset.
        if (app_.ui().resetLayout || !ImGui::DockBuilderGetNode(dockId) || ImGui::DockBuilderGetNode(dockId)->IsLeafNode())
            buildDefaultLayout(dockId);
        layoutBuilt_ = true;
        app_.ui().resetLayout = false;
    }
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
    statusBar();
}

void MainWindow::buildDefaultLayout(ImGuiID dockId) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);
    ImGuiID top, bottom, left, center, right, timeline, mixer;
    ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.40f, &bottom, &top);
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.24f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.36f, &right, &center);
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.14f, &mixer, &timeline);
    ImGui::DockBuilderDockWindow("###Media", left);
    ImGui::DockBuilderDockWindow("###Effects", left);
    ImGui::DockBuilderDockWindow("###Scopes", left);
    ImGui::DockBuilderDockWindow("###Viewer", center);
    ImGui::DockBuilderDockWindow("###Inspector", right);
    ImGui::DockBuilderDockWindow("###Color", right);
    ImGui::DockBuilderDockWindow("###Export", right);
    ImGui::DockBuilderDockWindow("###History", right);
    ImGui::DockBuilderDockWindow("###Markers", right);
    ImGui::DockBuilderDockWindow("###Diagnostics", right);
    ImGui::DockBuilderDockWindow("###Timeline", timeline);
    ImGui::DockBuilderDockWindow("###Mixer", mixer);
    ImGui::DockBuilderFinish(dockId);
}

void MainWindow::panelWindows() {
    UiState& ui = app_.ui();
    auto panel = [&](bool& show, const char* english, const char* id, auto&& body, ImGuiWindowFlags flags = 0) {
        if (!show) return;
        const std::string name = winName(english, id);
        if (ImGui::Begin(name.c_str(), &show, flags)) body();
        ImGui::End();
    };
    panel(ui.showMedia, "Media", "Media", [&] { media_.draw(app_); });
    panel(ui.showEffects, "Effects", "Effects", [&] { effects_.draw(app_); });
    panel(ui.showScopes, "Scopes", "Scopes", [&] { scopes_.draw(app_); });
    panel(ui.showViewer, "Viewer", "Viewer", [&] { viewer_.draw(app_); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    panel(ui.showInspector, "Inspector", "Inspector", [&] { inspector_.draw(app_); });
    panel(ui.showColor, "Color", "Color", [&] { color_.draw(app_); });
    panel(ui.showExport, "Export", "Export", [&] { export_.draw(app_); });
    panel(ui.showHistory, "History", "History", [&] { history_.draw(app_); });
    panel(ui.showMarkers, "Markers", "Markers", [&] { markers_.draw(app_); });
    panel(ui.showDiagnostics, "Diagnostics", "Diagnostics", [&] { diagnostics_.draw(app_); });
    panel(ui.showMixer, "Audio Mixer", "Mixer", [&] { mixer_.draw(app_); });
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

void MainWindow::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
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
            {"view.media", &ui.showMedia},     {"view.viewer", &ui.showViewer},   {"view.timeline", &ui.showTimeline},
            {"view.inspector", &ui.showInspector}, {"view.effects", &ui.showEffects}, {"view.color", &ui.showColor},
            {"view.mixer", &ui.showMixer},     {"view.scopes", &ui.showScopes},   {"view.export", &ui.showExport},
            {"view.markers", &ui.showMarkers}, {"view.history", &ui.showHistory}, {"view.diagnostics", &ui.showDiagnostics}};
        for (const auto& [id, flag] : views) menuItem(id, *flag);
        ImGui::Separator();
        menuItem("view.commandPalette");
        menuItem("view.resetLayout");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Help"))) {
        menuItem("help.about");
        menuItem("help.logs");
        menuItem("help.dataFolder");
        ImGui::EndMenu();
    }
    // Right side: project state.
    const std::string state = app_.doc().dirty() ? std::string(tr("Unsaved changes")) : std::string(tr("Saved"));
    const float w = ImGui::CalcTextSize(state.c_str()).x + ImGui::CalcTextSize(app_.project().name.c_str()).x + 40;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextDisabled("%s  -  %s", app_.project().name.c_str(), state.c_str());
    ImGui::EndMenuBar();
}

// ------------------------------------------------------------------ status bar

void MainWindow::statusBar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + 4;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImGui::AlignTextToFramePadding();
    std::string left = app_.statusText();
    if (left.empty()) left = app_.projectPath().empty() ? std::string(tr("Project not saved yet")) : app_.projectPath();
    ImGui::TextDisabled("%s", left.c_str());
    // Background work
    std::string mid;
    for (const auto& j : app_.exports().jobs()) {
        const auto p = j->progress();
        if (p.state == exp::ExportState::Rendering || p.state == exp::ExportState::Finalizing || p.state == exp::ExportState::Verifying) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s %.0f%%", tr("Exporting"), p.fraction() * 100.0f);
            mid = buf;
        }
    }
    const size_t jobs = Jobs::allActiveJobs().size();
    if (mid.empty() && jobs > 0) mid = std::to_string(jobs) + " " + tr("background task(s)");
    if (!mid.empty()) {
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.45f);
        ImGui::TextUnformatted(mid.c_str());
    }
    const auto f = app_.preview().latest();
    char right[160];
    std::snprintf(right, sizeof right, "%s%s  %dx%d  %.0f ms  |  %s", app_.stats.lowMemory ? "[!] " : "",
                  app_.settings().proxy.useProxies ? "Proxy " : "", f.width, f.height, f.renderMs, app_.device().info().adapter.c_str());
    const float rw = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - rw - 12);
    ImGui::TextDisabled("%s", right);
    ImGui::End();
}

// ------------------------------------------------------------------ notifications

void MainWindow::notifications() {
    auto& notes = app_.notifications();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - ImGui::GetFrameHeight() * 2.0f - 8;
    uint64_t dismiss = 0;
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const Notification& n = *it;
        if (n.level < LogLevel::Info) continue;  // debug-level notes only go to the log
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12, y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowBgAlpha(0.95f);
        const std::string id = "##note" + std::to_string(n.id);
        ImGui::PushStyleColor(ImGuiCol_Border, n.level >= LogLevel::Error ? IM_COL32(220, 70, 60, 255)
                                               : n.level == LogLevel::Warning ? IM_COL32(230, 170, 50, 255) : IM_COL32(70, 120, 200, 255));
        ImGui::Begin(id.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(n.text.c_str());
        ImGui::PopTextWrapPos();
        if (n.action && ImGui::SmallButton(n.actionLabel.c_str())) {
            n.action();
            dismiss = n.id;
        }
        if (n.action) ImGui::SameLine();
        if (ImGui::SmallButton(tr("Dismiss"))) dismiss = n.id;
        y -= ImGui::GetWindowHeight() + 6;
        ImGui::End();
        ImGui::PopStyleColor();
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
    ImGui::InputText(tr("Name"), newName_, sizeof newName_);
    ImGui::Combo(tr("Format"), &newPreset_, [](void*, int i) { return fmts[i].name; }, nullptr, 6);
    ImGui::Combo(tr("Frame Rate"), &newRate_, [](void*, int i) { return rates[i].first; }, nullptr, 8);
    ImGui::TextDisabled("%s", tr("The first video you import into an empty sequence can also set the format."));
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
    ImGui::Checkbox(tr("Ripple edit (shift following clips)"), &speedRipple_);
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
    ImGui::Text("AviCap Studio %s", AVICAP_VERSION_STRING);
    ImGui::TextDisabled("%s  |  %s", AVICAP_BUILD_DATE, AVICAP_COMPILER);
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Video editor for Windows. Local first: your media never leaves this PC unless you choose a cloud feature."));
    ImGui::TextDisabled("%s", tr("Uses FFmpeg (LGPL), Dear ImGui, nlohmann/json, stb, doctest. See THIRD_PARTY_LICENSES.md."));
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
    auto target = timeline_.dropTargetAt(app_, screenPos);
    if (!target) {
        app_.importFiles(paths);
        return;
    }
    const auto t = *target;
    App* app = &app_;
    app_.importFiles(paths, [app, t](const std::vector<MediaId>& ids) {
        Time at = t.time;
        for (MediaId id : ids) {
            auto r = app->addMediaToTimeline(id, at, t.videoTrack, t.audioTrack, false);
            if (!r || r->empty()) continue;
            if (const Clip* c = app->sequence()->clip(r->front())) at = c->end();
        }
    });
}

}  // namespace avc::ui
