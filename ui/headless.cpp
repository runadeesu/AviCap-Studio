#include "ui/headless.h"

namespace avc::ui {

HeadlessHost::HeadlessHost(App& app, ImVec2 displaySize) : app_(app) {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.DisplaySize = displaySize;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    loadUiFonts();
    applyTheme(1.0f, false);
    window_ = std::make_unique<MainWindow>(app);
}

HeadlessHost::~HeadlessHost() {
    ImGui::SetCurrentContext(ctx_);
    window_.reset();
    ImGui::DestroyContext(ctx_);
}

void HeadlessHost::frame(float dt) {
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt;
    app_.tick();
    ImGui::NewFrame();
    window_->frame();
    ImGui::Render();
    // Pretend to be a renderer that owns the font atlas textures.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status == ImTextureStatus_WantCreate) {
            tex->SetTexID(static_cast<ImTextureID>(1));
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    const ImDrawData* dd = ImGui::GetDrawData();
    lastDrawCmds_ = 0;
    lastVertices_ = dd ? dd->TotalVtxCount : 0;
    if (dd)
        for (const ImDrawList* l : dd->CmdLists) lastDrawCmds_ += l->CmdBuffer.Size;
}

void HeadlessHost::mouseMove(ImVec2 p) {
    ImGui::SetCurrentContext(ctx_);
    ImGui::GetIO().AddMousePosEvent(p.x, p.y);
}

void HeadlessHost::mouseButton(int button, bool down) {
    ImGui::SetCurrentContext(ctx_);
    ImGui::GetIO().AddMouseButtonEvent(button, down);
}

void HeadlessHost::click(ImVec2 p, int button) {
    mouseMove(p);
    frame();
    mouseButton(button, true);
    frame();
    mouseButton(button, false);
    frame();
}

void HeadlessHost::doubleClick(ImVec2 p) {
    mouseMove(p);
    frame();
    for (int i = 0; i < 2; ++i) {
        mouseButton(0, true);
        frame(0.01f);
        mouseButton(0, false);
        frame(0.01f);
    }
}

void HeadlessHost::drag(ImVec2 from, ImVec2 to, int steps, ImGuiKey modifier) {
    mouseMove(from);
    frame();
    if (modifier != ImGuiKey_None) key(modifier, true);
    mouseButton(0, true);
    frame();
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        mouseMove(ImVec2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t));
        frame();
    }
    mouseButton(0, false);
    frame();
    if (modifier != ImGuiKey_None) key(modifier, false);
    frame();
}

void HeadlessHost::key(ImGuiKey k, bool down) {
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    if (k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, down);
    if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) io.AddKeyEvent(ImGuiMod_Shift, down);
    if (k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt) io.AddKeyEvent(ImGuiMod_Alt, down);
    io.AddKeyEvent(k, down);
}

void HeadlessHost::press(ImGuiKey k, bool ctrl, bool shift, bool alt) {
    if (ctrl) key(ImGuiKey_LeftCtrl, true);
    if (shift) key(ImGuiKey_LeftShift, true);
    if (alt) key(ImGuiKey_LeftAlt, true);
    frame();
    key(k, true);
    frame();
    key(k, false);
    if (ctrl) key(ImGuiKey_LeftCtrl, false);
    if (shift) key(ImGuiKey_LeftShift, false);
    if (alt) key(ImGuiKey_LeftAlt, false);
    frame();
}

void HeadlessHost::wheel(float y, float x) {
    ImGui::SetCurrentContext(ctx_);
    ImGui::GetIO().AddMouseWheelEvent(x, y);
}

}  // namespace avc::ui
