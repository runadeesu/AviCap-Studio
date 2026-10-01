#pragma once
// Minimal UI localization. English is the key language; Japanese is built in.
// tr("Import Media") returns the translated string for the active language.

#include <string>
#include <string_view>

namespace avc {

enum class Language { English, Japanese };

void setLanguage(Language lang);
Language currentLanguage();
// "auto" chooses from the OS UI language.
Language languageFromSetting(std::string_view setting);

// Returns a pointer valid for the process lifetime (safe for ImGui labels).
const char* tr(const char* english);
// Marks a string for translation without translating it here (tables of
// labels translated later with tr(variable)); the i18n test scans for it.
constexpr const char* trNoop(const char* english) { return english; }
// Whether a translation exists (used by tests to catch untranslated UI text).
bool hasTranslation(std::string_view english, Language lang = Language::Japanese);

}  // namespace avc
