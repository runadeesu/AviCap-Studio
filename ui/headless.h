#pragma once
// Runs the complete editor UI without a window or GPU swapchain: real ImGui
// frames, real panels and real input handling, with synthesized mouse and
// keyboard events. Used by the UI tests (on every platform) and for
// automation.

#include <memory>

#include <imgui.h>

#include "ui/main_window.h"

namespace avc::ui {

class HeadlessHost {
public:
    explicit HeadlessHost(App& app, ImVec2 displaySize = ImVec2(1600, 900));
    ~HeadlessHost();
    HeadlessHost(const HeadlessHost&) = delete;
    HeadlessHost& operator=(const HeadlessHost&) = delete;

    // One full UI frame (App::tick + all panels + Render).
    void frame(float dt = 1.0f / 60.0f);
    void frames(int n) {
        for (int i = 0; i < n; ++i) frame();
    }

    void mouseMove(ImVec2 p);
    void mouseButton(int button, bool down);
    void click(ImVec2 p, int button = 0);  // move + press + release (3 frames)
    void doubleClick(ImVec2 p);
    void drag(ImVec2 from, ImVec2 to, int steps = 8, ImGuiKey modifier = ImGuiKey_None);
    void key(ImGuiKey k, bool down);
    void press(ImGuiKey k, bool ctrl = false, bool shift = false, bool alt = false);  // full key stroke
    void wheel(float y, float x = 0.0f);

    MainWindow& window() { return *window_; }
    ImGuiContext* context() { return ctx_; }
    [[nodiscard]] int drawCalls() const { return lastDrawCmds_; }
    [[nodiscard]] int vertices() const { return lastVertices_; }

private:
    App& app_;
    ImGuiContext* ctx_ = nullptr;
    std::unique_ptr<MainWindow> window_;
    int lastDrawCmds_ = 0;
    int lastVertices_ = 0;
};

}  // namespace avc::ui
