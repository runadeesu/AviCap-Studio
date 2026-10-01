#pragma once
// Action registry and keyboard shortcuts.
//
// Every user-invokable operation is an Action with a stable id
// ("timeline.split"). Menus, the command palette and keyboard dispatch all go
// through the registry, so the shortcut editor can rebind anything and show
// conflicts. Shortcuts come from a preset (avicap / premiere / resolve) plus
// per-action user overrides persisted in settings.

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace avc::ui {

struct KeyChord {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    [[nodiscard]] bool valid() const { return key != ImGuiKey_None; }
    [[nodiscard]] std::string toString() const;  // "Ctrl+Shift+K"
    static KeyChord parse(std::string_view text);  // invalid chord on error / empty
    bool operator==(const KeyChord&) const = default;
};

struct Action {
    std::string id;
    const char* label = "";  // English; translated with tr() for display
    std::string category;    // "File", "Edit", "Timeline", "Playback", ...
    // Default shortcut per keymap preset; "avicap" is the fallback.
    std::map<std::string, std::string> defaults;
    std::function<void()> run;
    std::function<bool()> enabled;  // optional
    bool repeat = false;            // fires on key auto-repeat (frame stepping)
};

class CommandRegistry {
public:
    void add(Action a);
    [[nodiscard]] const Action* find(std::string_view id) const;
    [[nodiscard]] const std::vector<Action>& actions() const { return actions_; }
    [[nodiscard]] bool isEnabled(std::string_view id) const;
    // Runs an action if it exists and is enabled. Returns true when it ran.
    bool run(std::string_view id);

    void setPreset(std::string preset);
    [[nodiscard]] const std::string& preset() const { return preset_; }
    [[nodiscard]] KeyChord defaultShortcut(std::string_view id) const;
    [[nodiscard]] KeyChord shortcut(std::string_view id) const;  // effective
    [[nodiscard]] std::string shortcutText(std::string_view id) const;
    // An invalid chord removes the shortcut (explicitly unbound).
    void setOverride(const std::string& id, KeyChord chord);
    void clearOverride(const std::string& id);
    void resetAll() { overrides_.clear(); }
    [[nodiscard]] bool hasOverride(std::string_view id) const;
    // Other actions bound to the same chord.
    [[nodiscard]] std::vector<std::string> conflicts(const KeyChord& chord, std::string_view exceptId = {}) const;
    [[nodiscard]] std::string actionForChord(const KeyChord& chord) const;

    void loadOverrides(const std::map<std::string, std::string>& o);
    [[nodiscard]] std::map<std::string, std::string> saveOverrides() const;

    // Checks the current ImGui keyboard state and runs the matching action.
    // Call once per frame on the UI thread. Does nothing while a text field
    // has focus. Returns the id of the action that ran, if any.
    std::string processKeyboard();

private:
    std::vector<Action> actions_;
    std::map<std::string, size_t, std::less<>> index_;
    std::map<std::string, KeyChord, std::less<>> overrides_;  // invalid chord = unbound
    std::string preset_ = "avicap";
};

}  // namespace avc::ui
