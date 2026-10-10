#include "ui/headless.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <stb_image_write.h>

#include "core/strings.h"
#include "render/gpu/gpu.h"

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
    accent_ = app.settings().ui.accent;
    applyTheme(1.0f, false, accent_);
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
    if (app_.settings().ui.accent != accent_) {
        accent_ = app_.settings().ui.accent;
        applyTheme(1.0f, false, accent_);
    }
    ImGui::NewFrame();
    window_->frame();
    ImGui::Render();
    // Pretend to be a renderer that owns the font atlas textures.
    // Small ids cannot collide with the Texture* host views of user images.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status == ImTextureStatus_WantCreate) {
            const auto id = static_cast<ImTextureID>(nextTexId_++);
            tex->SetTexID(id);
            fontTextures_[id] = tex;
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            fontTextures_.erase(tex->GetTexID());
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

// ------------------------------------------------------------------ software rasterizer

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight alpha
    bool alphaOnly = false;
};

inline float edge(const ImVec2& a, const ImVec2& b, float x, float y) { return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x); }

void sample(const Image* img, float u, float v, float out[4]) {
    if (!img || img->w == 0) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    // Bilinear (font atlas texels are tiny; images are scaled).
    const float fx = std::clamp(u * img->w - 0.5f, 0.0f, static_cast<float>(img->w - 1));
    const float fy = std::clamp(v * img->h - 0.5f, 0.0f, static_cast<float>(img->h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, img->w - 1), y1 = std::min(y0 + 1, img->h - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    auto px = [&](int x, int y, int c) { return img->rgba[(static_cast<size_t>(y) * static_cast<size_t>(img->w) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] / 255.0f; };
    for (int c = 0; c < 4; ++c) {
        const float a = px(x0, y0, c) + (px(x1, y0, c) - px(x0, y0, c)) * tx;
        const float b = px(x0, y1, c) + (px(x1, y1, c) - px(x0, y1, c)) * tx;
        out[c] = a + (b - a) * ty;
    }
}

}  // namespace

bool HeadlessHost::saveScreenshot(const std::string& path) {
    ImGui::SetCurrentContext(ctx_);
    const ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || !dd->Valid) return false;
    const int W = static_cast<int>(dd->DisplaySize.x), H = static_cast<int>(dd->DisplaySize.y);
    if (W <= 0 || H <= 0) return false;
    std::vector<float> fb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3, 0.0f);
    const ImVec4 clear = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    for (size_t i = 0; i < fb.size(); i += 3) {
        fb[i] = clear.x;
        fb[i + 1] = clear.y;
        fb[i + 2] = clear.z;
    }
    // Textures: font atlases (ImTextureData) and device textures (Texture*).
    std::unordered_map<ImTextureID, Image> images;
    auto image = [&](ImTextureID id) -> const Image* {
        auto it = images.find(id);
        if (it != images.end()) return &it->second;
        Image img;
        if (auto f = fontTextures_.find(id); f != fontTextures_.end()) {
            ImTextureData* t = f->second;
            img.w = t->Width;
            img.h = t->Height;
            img.rgba.resize(static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4);
            const auto* src = static_cast<const uint8_t*>(t->GetPixels());
            for (size_t i = 0, n = static_cast<size_t>(img.w) * static_cast<size_t>(img.h); i < n; ++i) {
                if (t->Format == ImTextureFormat_Alpha8) {
                    img.rgba[i * 4] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = 255;
                    img.rgba[i * 4 + 3] = src[i];
                } else {
                    std::copy_n(src + i * 4, 4, img.rgba.data() + i * 4);
                }
            }
        } else if (id != ImTextureID_Invalid) {
            const auto* tex = reinterpret_cast<const gpu::Texture*>(id);
            img.w = tex->width();
            img.h = tex->height();
            img.rgba = gpu::readbackRgba8(app_.device(), *tex, true);
            if (img.rgba.size() != static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4) img = {};
        }
        return &images.emplace(id, std::move(img)).first->second;
    };
    for (const ImDrawList* list : dd->CmdLists) {
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const Image* img = image(cmd.GetTexID());
            const float cx0 = std::max(0.0f, cmd.ClipRect.x - dd->DisplayPos.x), cy0 = std::max(0.0f, cmd.ClipRect.y - dd->DisplayPos.y);
            const float cx1 = std::min(static_cast<float>(W), cmd.ClipRect.z - dd->DisplayPos.x);
            const float cy1 = std::min(static_cast<float>(H), cmd.ClipRect.w - dd->DisplayPos.y);
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            for (unsigned int e = 0; e + 2 < cmd.ElemCount; e += 3) {
                const ImDrawVert& a = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + e]];
                const ImDrawVert& b = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + e + 1]];
                const ImDrawVert& c = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + e + 2]];
                const ImVec2 pa(a.pos.x - dd->DisplayPos.x, a.pos.y - dd->DisplayPos.y);
                const ImVec2 pb(b.pos.x - dd->DisplayPos.x, b.pos.y - dd->DisplayPos.y);
                const ImVec2 pc(c.pos.x - dd->DisplayPos.x, c.pos.y - dd->DisplayPos.y);
                const float area = edge(pa, pb, pc.x, pc.y);
                if (std::fabs(area) < 1e-6f) continue;
                const int x0 = static_cast<int>(std::floor(std::max(cx0, std::min({pa.x, pb.x, pc.x}))));
                const int x1 = static_cast<int>(std::ceil(std::min(cx1, std::max({pa.x, pb.x, pc.x}))));
                const int y0 = static_cast<int>(std::floor(std::max(cy0, std::min({pa.y, pb.y, pc.y}))));
                const int y1 = static_cast<int>(std::ceil(std::min(cy1, std::max({pa.y, pb.y, pc.y}))));
                // Solid-colour triangles (white texel, same colour) skip sampling.
                const bool flat = a.col == b.col && b.col == c.col;
                for (int y = y0; y < y1; ++y) {
                    const float py = static_cast<float>(y) + 0.5f;
                    if (py < cy0 || py >= cy1) continue;
                    for (int x = x0; x < x1; ++x) {
                        const float px = static_cast<float>(x) + 0.5f;
                        if (px < cx0 || px >= cx1) continue;
                        float w0 = edge(pb, pc, px, py) / area, w1 = edge(pc, pa, px, py) / area, w2 = edge(pa, pb, px, py) / area;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        const float u = a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2;
                        const float v = a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2;
                        float col[4];
                        if (flat) {
                            for (int k = 0; k < 4; ++k) col[k] = static_cast<float>((a.col >> (8 * k)) & 0xFF) / 255.0f;
                        } else {
                            for (int k = 0; k < 4; ++k)
                                col[k] = (static_cast<float>((a.col >> (8 * k)) & 0xFF) * w0 + static_cast<float>((b.col >> (8 * k)) & 0xFF) * w1 +
                                          static_cast<float>((c.col >> (8 * k)) & 0xFF) * w2) / 255.0f;
                        }
                        float tex[4];
                        sample(img, u, v, tex);
                        const float alpha = col[3] * tex[3];
                        if (alpha <= 0.0f) continue;
                        float* dst = fb.data() + (static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 3;
                        for (int k = 0; k < 3; ++k) dst[k] = dst[k] * (1.0f - alpha) + col[k] * tex[k] * alpha;
                    }
                }
            }
        }
    }
    std::vector<uint8_t> out(static_cast<size_t>(W) * static_cast<size_t>(H) * 3);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(std::clamp(fb[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return stbi_write_png(path.c_str(), W, H, 3, out.data(), W * 3) != 0;
}

}  // namespace avc::ui
