// Dark editor theme (plus a high-contrast variant) and system font loading.

#include <cfloat>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include <stb_truetype.h>

#include "core/file_io.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "ui/main_window.h"
#include "ui/style.h"

namespace avc::ui {

namespace pal {

namespace {
struct AccentDef {
    AccentPreset preset;
    ImU32 hover, pressed, text, second;
};
// Each accent keeps white text readable on the filled buttons.
const AccentDef kAccents[] = {
    {{"violet", trNoop("Violet"), IM_COL32(124, 92, 255, 255)}, IM_COL32(145, 118, 255, 255), IM_COL32(104, 74, 230, 255),
     IM_COL32(178, 160, 255, 255), IM_COL32(56, 160, 255, 255)},
    {{"cyan", trNoop("Cyan"), IM_COL32(0, 172, 200, 255)}, IM_COL32(20, 196, 224, 255), IM_COL32(0, 148, 174, 255),
     IM_COL32(96, 222, 240, 255), IM_COL32(64, 120, 255, 255)},
    {{"blue", trNoop("Blue"), IM_COL32(40, 120, 235, 255)}, IM_COL32(66, 142, 245, 255), IM_COL32(30, 100, 210, 255),
     IM_COL32(130, 180, 255, 255), IM_COL32(32, 200, 220, 255)},
    {{"pink", trNoop("Pink"), IM_COL32(230, 60, 140, 255)}, IM_COL32(242, 92, 162, 255), IM_COL32(204, 46, 122, 255),
     IM_COL32(250, 160, 205, 255), IM_COL32(255, 140, 60, 255)},
    {{"orange", trNoop("Orange"), IM_COL32(236, 104, 28, 255)}, IM_COL32(248, 128, 52, 255), IM_COL32(212, 88, 18, 255),
     IM_COL32(255, 178, 120, 255), IM_COL32(236, 60, 120, 255)},
};
}  // namespace

const AccentPreset* accentPresets(int* count) {
    static AccentPreset list[std::size(kAccents)];
    for (size_t i = 0; i < std::size(kAccents); ++i) list[i] = kAccents[i].preset;
    if (count) *count = static_cast<int>(std::size(kAccents));
    return list;
}

void setAccent(const char* id) {
    const AccentDef* def = &kAccents[0];
    for (const AccentDef& d : kAccents)
        if (id && std::string_view(id) == d.preset.id) def = &d;
    kAccent = def->preset.swatch;
    kAccentHover = def->hover;
    kAccentPressed = def->pressed;
    kAccentSoft = withAlpha(def->preset.swatch, 46.0f / 255.0f);
    kAccentText = def->text;
    kAccent2 = def->second;
}

}  // namespace pal

void applyTheme(float scale, bool highContrast, const std::string& accent) {
    using namespace pal;
    setAccent(accent.c_str());
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    // Geometry: rounded, airy controls; panels are separated by gaps instead
    // of borders (the dock splitters show the app background).
    style.WindowRounding = kPanelRadius;
    style.ChildRounding = 8.0f;
    style.FrameRounding = kControlRadius;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 10.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabRounding = 7.0f;
    style.GrabMinSize = 12.0f;
    style.TabRounding = 7.0f;
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(8, 7);
    style.ItemInnerSpacing = ImVec2(6, 5);
    style.CellPadding = ImVec2(8, 5);
    style.IndentSpacing = 16.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBarOverlineSize = 2.0f;
    style.TabCloseButtonMinWidthSelected = 0.0f;     // close button only on hover
    style.TabCloseButtonMinWidthUnselected = FLT_MAX;  // never on inactive tabs
    style.DockingSeparatorSize = kPanelGap;
    style.DockingNodeHasCloseButton = false;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(0, 4);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = v4(kText);
    c[ImGuiCol_TextDisabled] = v4(kTextMuted);
    c[ImGuiCol_WindowBg] = v4(kPanel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.115f, 0.122f, 0.145f, 0.985f);
    c[ImGuiCol_Border] = v4(kStroke);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v4(kPanelRaised);
    c[ImGuiCol_FrameBgHovered] = v4(kPanelHover);
    c[ImGuiCol_FrameBgActive] = v4(kPanelActive);
    c[ImGuiCol_TitleBg] = v4(kPanel);
    c[ImGuiCol_TitleBgActive] = v4(kPanel);
    c[ImGuiCol_TitleBgCollapsed] = v4(kPanel);
    c[ImGuiCol_MenuBarBg] = v4(kAppBg);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_ScrollbarGrabActive] = v4(kAccent);
    c[ImGuiCol_CheckMark] = v4(kAccentHover);
    c[ImGuiCol_SliderGrab] = v4(kAccent);
    c[ImGuiCol_SliderGrabActive] = v4(kAccentHover);
    c[ImGuiCol_Button] = v4(kPanelRaised);
    c[ImGuiCol_ButtonHovered] = v4(kPanelHover);
    c[ImGuiCol_ButtonActive] = v4(kPanelActive);
    c[ImGuiCol_Header] = v4(withAlpha(kAccent, 0.22f));
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_HeaderActive] = v4(withAlpha(kAccent, 0.32f));
    c[ImGuiCol_Separator] = v4(kStroke);
    c[ImGuiCol_SeparatorHovered] = v4(withAlpha(kAccent, 0.6f));
    c[ImGuiCol_SeparatorActive] = v4(kAccent);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = v4(withAlpha(kAccent, 0.6f));
    c[ImGuiCol_ResizeGripActive] = v4(kAccent);
    c[ImGuiCol_InputTextCursor] = v4(kText);
    c[ImGuiCol_Tab] = v4(kPanel);
    c[ImGuiCol_TabHovered] = v4(kPanelHover);
    c[ImGuiCol_TabSelected] = v4(kPanel);
    c[ImGuiCol_TabSelectedOverline] = v4(kAccent);
    c[ImGuiCol_TabDimmed] = v4(kPanel);
    c[ImGuiCol_TabDimmedSelected] = v4(kPanel);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_DockingPreview] = v4(withAlpha(kAccent, 0.45f));
    c[ImGuiCol_DockingEmptyBg] = v4(kAppBg);
    c[ImGuiCol_PlotLines] = v4(kAccentHover);
    c[ImGuiCol_PlotHistogram] = v4(kAccent);
    c[ImGuiCol_TableHeaderBg] = v4(kPanelRaised);
    c[ImGuiCol_TableBorderStrong] = v4(kStroke);
    c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.04f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextLink] = v4(kAccentText);
    c[ImGuiCol_TextSelectedBg] = v4(withAlpha(kAccent, 0.40f));
    c[ImGuiCol_DragDropTarget] = v4(kAccentHover);
    c[ImGuiCol_NavCursor] = v4(kAccent);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.04f, 0.62f);
    if (highContrast) {
        c[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.82f, 0.82f, 0.82f, 1);
        c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 1);
        c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.8f);
        c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.10f, 1);
        c[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.16f, 1);
        c[ImGuiCol_Header] = ImVec4(0.30f, 0.20f, 0.85f, 1);
        style.FrameBorderSize = 1.0f;
    }
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 16.0f;
    ImGui::GetStyle() = style;
}

namespace {

ImFont* g_bold = nullptr;

namespace fs = std::filesystem;

std::vector<fs::path> fontDirs() {
    std::vector<fs::path> dirs;
    if (auto d = getEnv("AVICAP_FONT_DIR")) dirs.push_back(pathFromUtf8(*d));
#if defined(_WIN32)
    dirs.push_back(pathFromUtf8(getEnv("WINDIR").value_or("C:\\Windows")) / "Fonts");
    if (auto local = getEnv("LOCALAPPDATA")) dirs.push_back(pathFromUtf8(*local) / "Microsoft" / "Windows" / "Fonts");
#else
    for (const char* d : {"/usr/share/fonts", "/usr/local/share/fonts"}) dirs.emplace_back(d);
    if (auto home = getEnv("HOME")) dirs.push_back(pathFromUtf8(*home) / ".local/share/fonts");
#endif
    return dirs;
}

// Finds a font file by name in the font folders (recursively on Linux).
std::optional<fs::path> findFont(const char* file) {
    std::error_code ec;
    for (const auto& d : fontDirs()) {
        const fs::path direct = d / file;
        if (fs::exists(direct, ec)) return direct;
#if !defined(_WIN32)
        for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename() == file) return it->path();
#endif
    }
    return std::nullopt;
}

// Face index inside a .ttc collection whose family name matches `family`
// (e.g. "Yu Gothic UI" inside YuGothR.ttc); -1 when absent.
int faceIndex(const fs::path& file, const char* family) {
    auto bytes = readFileBytes(file);
    if (!bytes) return -1;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes->data());
    const int n = std::max(1, stbtt_GetNumberOfFonts(data));
    for (int i = 0; i < n; ++i) {
        const int off = stbtt_GetFontOffsetForIndex(data, i);
        stbtt_fontinfo info;
        if (off < 0 || !stbtt_InitFont(&info, data, off)) continue;
        int len = 0;
        // Name id 1 (family), Windows platform, Unicode BMP, English (US).
        const char* name = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
        if (!name) continue;
        std::string ascii;
        for (int k = 1; k < len; k += 2) ascii += name[k];  // UTF-16BE, ASCII names
        if (ascii == family) return i;
    }
    return -1;
}

struct Face {
    const char* file;
    const char* family;  // nullptr: first face
};

bool addFace(const Face& f, bool merge, std::string& used, ImFont** out = nullptr) {
    auto path = findFont(f.file);
    if (!path) return false;
    int index = 0;
    if (f.family) {
        index = faceIndex(*path, f.family);
        if (index < 0) return false;
    }
    ImFontConfig cfg;
    cfg.MergeMode = merge;
    cfg.FontNo = index;
    cfg.OversampleH = 2;
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(pathToUtf8(*path).c_str(), 0.0f, &cfg);
    if (!font) return false;
    if (out) *out = font;
    used += std::string(used.empty() ? "" : " + ") + (f.family ? f.family : f.file);
    return true;
}

bool addFirst(const std::vector<Face>& list, bool merge, std::string& used, ImFont** out = nullptr) {
    for (const auto& f : list)
        if (addFace(f, merge, used, out)) return true;
    return false;
}

}  // namespace

ImFont* boldFont() { return g_bold; }

std::string loadUiFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Modern Windows UI faces first: Segoe UI Variable / Segoe UI for Latin
    // and digits, Yu Gothic UI (or Meiryo UI) merged in for Japanese.
    const std::vector<Face> latin = {{"SegUIVar.ttf", nullptr}, {"segoeui.ttf", nullptr}, {"DejaVuSans.ttf", nullptr},
                                     {"LiberationSans-Regular.ttf", nullptr}, {"arial.ttf", nullptr}};
    const std::vector<Face> latinBold = {{"seguisb.ttf", nullptr}, {"segoeuib.ttf", nullptr}, {"DejaVuSans-Bold.ttf", nullptr},
                                         {"LiberationSans-Bold.ttf", nullptr}, {"arialbd.ttf", nullptr}};
    const std::vector<Face> japanese = {{"YuGothM.ttc", "Yu Gothic UI"}, {"YuGothR.ttc", "Yu Gothic UI"}, {"meiryo.ttc", "Meiryo UI"},
                                        {"BIZ-UDGothicR.ttc", "BIZ UDPGothic"}, {"NotoSansJP-Regular.otf", nullptr},
                                        {"NotoSansCJK-Regular.ttc", nullptr}, {"ipaexg.ttf", nullptr}, {"wqy-zenhei.ttc", nullptr},
                                        {"msgothic.ttc", nullptr}};
    const std::vector<Face> japaneseBold = {{"YuGothB.ttc", "Yu Gothic UI"}, {"meiryob.ttc", "Meiryo UI"},
                                            {"BIZ-UDGothicB.ttc", "BIZ UDPGothic"}, {"NotoSansJP-Bold.otf", nullptr},
                                            {"ipaexg.ttf", nullptr}, {"wqy-zenhei.ttc", nullptr}};
    std::string used;
    bool haveMain = addFirst(latin, false, used);
    if (haveMain) addFirst(japanese, true, used);
    else haveMain = addFirst(japanese, false, used);
    if (!haveMain) {
        io.Fonts->AddFontDefault();
        used = "ProggyClean (built-in)";
    }
    std::string boldUsed;
    g_bold = nullptr;
    if (addFirst(latinBold, false, boldUsed, &g_bold)) addFirst(japaneseBold, true, boldUsed);
    AVC_INFO("ui", "UI fonts: {} / bold: {}", used, boldUsed.empty() ? "none" : boldUsed);
    return used;
}

}  // namespace avc::ui
