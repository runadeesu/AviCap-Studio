#pragma once
// Top-level editor UI for one frame: docking layout, menus, status bar,
// notifications, modal dialogs and the panels. Platform independent; the
// Win32 host (or the headless test host) wraps it in NewFrame/Render.

#include <string>
#include <vector>

#include <imgui.h>

#include "ui/panels.h"

namespace avc::ui {

void applyTheme(float scale, bool highContrast);
// Loads UI fonts (Latin + Japanese) from the system; returns a description.
std::string loadUiFonts();

class MainWindow {
public:
    explicit MainWindow(App& app);

    void frame();
    // OS file drop at a screen position (imports, and places on the timeline
    // when dropped over the lanes).
    void onFilesDropped(const std::vector<std::string>& paths, ImVec2 screenPos);
    TimelinePanel& timeline() { return timeline_; }
    ViewerPanel& viewer() { return viewer_; }
    // Window title changes are applied by the host.
    [[nodiscard]] std::string title() const { return app_.windowTitle(); }

private:
    void menuBar();
    void menuItem(const char* id, bool checked = false);
    void dockspace();
    void buildDefaultLayout(ImGuiID dockId);
    void statusBar();
    void notifications();
    void modals();
    void unsavedDialog();
    void newProjectDialog();
    void recoveryDialog();
    void relinkDialog();
    void speedDialog();
    void aboutDialog();
    void commandPalette();
    void fullscreenViewer();
    void panelWindows();

    App& app_;
    TimelinePanel timeline_;
    MediaPanel media_;
    ViewerPanel viewer_;
    InspectorPanel inspector_;
    EffectsPanel effects_;
    ColorPanel color_;
    ExportPanel export_;
    MixerPanel mixer_;
    ScopesPanel scopes_;
    HistoryPanel history_;
    MarkersPanel markers_;
    DiagnosticsPanel diagnostics_;
    SettingsWindow settings_;

    bool layoutBuilt_ = false;
    // New project dialog
    char newName_[128] = {};
    int newPreset_ = 0;
    int newRate_ = 4;
    // Speed dialog
    float speedPct_ = 100.0f;
    bool speedRipple_ = true;
    // Command palette
    char paletteQuery_[128] = {};
    int paletteIndex_ = 0;
};

}  // namespace avc::ui
