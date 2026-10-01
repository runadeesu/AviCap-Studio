#include "ui/commands.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace avc::ui {

namespace {

struct KeyName {
    ImGuiKey key;
    const char* name;
};

// Names used in settings files (stable, English, independent of ImGui's names).
const KeyName kKeyNames[] = {
    {ImGuiKey_Space, "Space"},       {ImGuiKey_Enter, "Enter"},         {ImGuiKey_Escape, "Esc"},
    {ImGuiKey_Tab, "Tab"},           {ImGuiKey_Backspace, "Backspace"}, {ImGuiKey_Delete, "Delete"},
    {ImGuiKey_Insert, "Insert"},     {ImGuiKey_Home, "Home"},           {ImGuiKey_End, "End"},
    {ImGuiKey_PageUp, "PageUp"},     {ImGuiKey_PageDown, "PageDown"},   {ImGuiKey_LeftArrow, "Left"},
    {ImGuiKey_RightArrow, "Right"},  {ImGuiKey_UpArrow, "Up"},          {ImGuiKey_DownArrow, "Down"},
    {ImGuiKey_Minus, "-"},           {ImGuiKey_Equal, "="},             {ImGuiKey_LeftBracket, "["},
    {ImGuiKey_RightBracket, "]"},    {ImGuiKey_Semicolon, ";"},         {ImGuiKey_Apostrophe, "'"},
    {ImGuiKey_Comma, ","},           {ImGuiKey_Period, "."},            {ImGuiKey_Slash, "/"},
    {ImGuiKey_Backslash, "\\"},      {ImGuiKey_GraveAccent, "`"},       {ImGuiKey_KeypadAdd, "Num+"},
    {ImGuiKey_KeypadSubtract, "Num-"}, {ImGuiKey_KeypadEnter, "NumEnter"},
};

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

const char* keyName(ImGuiKey k) {
    static thread_local char buf[8];
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) {
        buf[0] = static_cast<char>('A' + (k - ImGuiKey_A));
        buf[1] = 0;
        return buf;
    }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) {
        buf[0] = static_cast<char>('0' + (k - ImGuiKey_0));
        buf[1] = 0;
        return buf;
    }
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) {
        std::snprintf(buf, sizeof buf, "F%d", 1 + (k - ImGuiKey_F1));
        return buf;
    }
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) {
        std::snprintf(buf, sizeof buf, "Num%d", k - ImGuiKey_Keypad0);
        return buf;
    }
    for (const auto& kn : kKeyNames)
        if (kn.key == k) return kn.name;
    return nullptr;
}

ImGuiKey keyFromName(std::string_view name) {
    const std::string u = upper(name);
    if (u.size() == 1 && u[0] >= 'A' && u[0] <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (u[0] - 'A'));
    if (u.size() == 1 && u[0] >= '0' && u[0] <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (u[0] - '0'));
    if (u.size() >= 2 && u[0] == 'F' && std::isdigit(static_cast<unsigned char>(u[1]))) {
        const int n = std::atoi(u.c_str() + 1);
        if (n >= 1 && n <= 12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
    }
    if (u.size() == 4 && u.rfind("NUM", 0) == 0 && std::isdigit(static_cast<unsigned char>(u[3])))
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (u[3] - '0'));
    for (const auto& kn : kKeyNames)
        if (upper(kn.name) == u) return kn.key;
    if (u == "ESCAPE") return ImGuiKey_Escape;
    if (u == "DEL") return ImGuiKey_Delete;
    if (u == "RETURN") return ImGuiKey_Enter;
    return ImGuiKey_None;
}

}  // namespace

std::string KeyChord::toString() const {
    if (!valid()) return {};
    std::string s;
    if (ctrl) s += "Ctrl+";
    if (shift) s += "Shift+";
    if (alt) s += "Alt+";
    const char* n = keyName(key);
    s += n ? n : "?";
    return s;
}

KeyChord KeyChord::parse(std::string_view text) {
    KeyChord c;
    if (text.empty()) return c;
    // Split on '+', but a trailing "+" key ("Ctrl++") is not supported; use "=" / "Num+".
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t p = text.find('+', start);
        if (p == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, p - start));
        start = p + 1;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string u = upper(parts[i]);
        if (i + 1 < parts.size()) {
            if (u == "CTRL" || u == "CONTROL") c.ctrl = true;
            else if (u == "SHIFT") c.shift = true;
            else if (u == "ALT") c.alt = true;
            else return {};
        } else {
            c.key = keyFromName(parts[i]);
        }
    }
    if (!c.valid()) return {};
    return c;
}

// ------------------------------------------------------------------ registry

void CommandRegistry::add(Action a) {
    if (auto it = index_.find(a.id); it != index_.end()) {
        actions_[it->second] = std::move(a);
        return;
    }
    index_[a.id] = actions_.size();
    actions_.push_back(std::move(a));
}

const Action* CommandRegistry::find(std::string_view id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &actions_[it->second];
}

bool CommandRegistry::isEnabled(std::string_view id) const {
    const Action* a = find(id);
    return a && a->run && (!a->enabled || a->enabled());
}

bool CommandRegistry::run(std::string_view id) {
    const Action* a = find(id);
    if (!a || !a->run) {
        AVC_WARN("ui", "unknown action '{}'", id);
        return false;
    }
    if (a->enabled && !a->enabled()) return false;
    AVC_DEBUG("ui", "action {}", id);
    // Copy: the action may replace registry entries (e.g. preset changes).
    auto fn = a->run;
    fn();
    return true;
}

void CommandRegistry::setPreset(std::string preset) {
    if (preset != "premiere" && preset != "resolve") preset = "avicap";
    preset_ = std::move(preset);
}

KeyChord CommandRegistry::defaultShortcut(std::string_view id) const {
    const Action* a = find(id);
    if (!a) return {};
    if (auto it = a->defaults.find(preset_); it != a->defaults.end()) return KeyChord::parse(it->second);
    if (auto it = a->defaults.find("avicap"); it != a->defaults.end()) return KeyChord::parse(it->second);
    return {};
}

KeyChord CommandRegistry::shortcut(std::string_view id) const {
    if (auto it = overrides_.find(id); it != overrides_.end()) return it->second;
    return defaultShortcut(id);
}

std::string CommandRegistry::shortcutText(std::string_view id) const { return shortcut(id).toString(); }

void CommandRegistry::setOverride(const std::string& id, KeyChord chord) {
    if (chord == defaultShortcut(id)) {
        overrides_.erase(id);
        return;
    }
    overrides_[id] = chord;
}

void CommandRegistry::clearOverride(const std::string& id) { overrides_.erase(id); }

bool CommandRegistry::hasOverride(std::string_view id) const { return overrides_.find(id) != overrides_.end(); }

std::vector<std::string> CommandRegistry::conflicts(const KeyChord& chord, std::string_view exceptId) const {
    std::vector<std::string> out;
    if (!chord.valid()) return out;
    for (const auto& a : actions_)
        if (a.id != exceptId && shortcut(a.id) == chord) out.push_back(a.id);
    return out;
}

std::string CommandRegistry::actionForChord(const KeyChord& chord) const {
    if (!chord.valid()) return {};
    for (const auto& a : actions_)
        if (shortcut(a.id) == chord) return a.id;
    return {};
}

void CommandRegistry::loadOverrides(const std::map<std::string, std::string>& o) {
    overrides_.clear();
    for (const auto& [id, text] : o) overrides_[id] = KeyChord::parse(text);  // "" = unbound
}

std::map<std::string, std::string> CommandRegistry::saveOverrides() const {
    std::map<std::string, std::string> out;
    for (const auto& [id, chord] : overrides_) out[id] = chord.toString();
    return out;
}

std::string CommandRegistry::processKeyboard() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return {};
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return {};
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    for (const auto& a : actions_) {
        const KeyChord c = shortcut(a.id);
        if (!c.valid() || c.ctrl != ctrl || c.shift != shift || c.alt != alt) continue;
        if (!ImGui::IsKeyPressed(c.key, a.repeat)) continue;
        const std::string id = a.id;
        if (run(id)) return id;
    }
    return {};
}

}  // namespace avc::ui
