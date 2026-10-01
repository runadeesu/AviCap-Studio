// Dark editor theme (plus a high-contrast variant) and system font loading.

#include <filesystem>

#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "ui/main_window.h"

namespace avc::ui {

void applyTheme(float scale, bool highContrast) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 4.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(6, 5);
    style.WindowBorderSize = 1.0f;
    style.DockingSeparatorSize = 3.0f;
    ImVec4* c = style.Colors;
    const ImVec4 bg(0.105f, 0.108f, 0.122f, 1.0f);
    const ImVec4 panel(0.135f, 0.138f, 0.155f, 1.0f);
    const ImVec4 accent(0.27f, 0.47f, 0.80f, 1.0f);
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.185f, 0.21f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.24f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.28f, 0.34f, 1.0f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.30f, 0.44f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.36f, 0.54f, 1.0f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.30f, 0.37f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.55f, 0.85f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.36f, 0.54f, 1.0f);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.27f, 0.47f, 0.80f, 0.45f);
    c[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 0.85f, 0.25f, 1.0f);
    if (highContrast) {
        c[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.80f, 0.80f, 0.80f, 1);
        c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 1);
        c[ImGuiCol_Border] = ImVec4(1, 1, 1, 1);
        c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.10f, 1);
        c[ImGuiCol_Button] = ImVec4(0.15f, 0.15f, 0.15f, 1);
        c[ImGuiCol_Header] = ImVec4(0.0f, 0.35f, 0.8f, 1);
        style.FrameBorderSize = 1.0f;
    }
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 15.0f;
    ImGui::GetStyle() = style;
}

std::string loadUiFonts() {
    ImGuiIO& io = ImGui::GetIO();
#if defined(_WIN32)
    namespace fs = std::filesystem;
    fs::path fonts = pathFromUtf8(getEnv("WINDIR").value_or("C:\\Windows")) / "Fonts";
    std::string used;
    auto exists = [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec);
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ImFont* main = nullptr;
    for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf"}) {
        if (exists(fonts / name)) {
            main = io.Fonts->AddFontFromFileTTF(pathToUtf8(fonts / name).c_str(), 0.0f, &cfg);
            if (main) {
                used = name;
                break;
            }
        }
    }
    if (!main) {
        io.Fonts->AddFontDefault();
        used = "ProggyClean (built-in)";
    }
    // Japanese glyphs (kana/kanji) merged into the main font.
    ImFontConfig merge;
    merge.MergeMode = true;
    for (const char* name : {"YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc", "NotoSansJP-Regular.otf",
                             "NotoSansCJK-Regular.ttc", "ipaexg.ttf"}) {
        if (exists(fonts / name) && io.Fonts->AddFontFromFileTTF(pathToUtf8(fonts / name).c_str(), 0.0f, &merge)) {
            used += std::string(" + ") + name;
            break;
        }
    }
    AVC_INFO("ui", "UI fonts: {}", used);
    return used;
#else
    io.Fonts->AddFontDefault();
    return "ProggyClean (built-in)";
#endif
}

}  // namespace avc::ui
