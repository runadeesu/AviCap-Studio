// Every user-visible string must have a Japanese translation. Strings are
// collected from the live registries (actions, effects, transitions,
// parameters, presets, enum names) and from tr("...") calls in the UI sources.

#include <doctest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "audio/dsp.h"
#include "core/i18n.h"
#include "effects/effects.h"
#include "export/export_settings.h"
#include "tests/test_support.h"
#include "ui/app.h"

using namespace avc;

namespace {

std::set<std::string> trLiteralsInSources() {
    std::set<std::string> out;
    const std::filesystem::path root = AVICAP_SOURCE_DIR;
    const std::regex call(R"re(\b(?:tr|fmt|trNoop)\(\s*"((?:[^"\\]|\\.)*)")re");
    for (const char* dir : {"ui", "apps/studio", "ai"}) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(root / dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".cpp") continue;
            std::ifstream f(it->path());
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string text = ss.str();
            for (std::sregex_iterator m(text.begin(), text.end(), call), mend; m != mend; ++m) {
                std::string lit = (*m)[1].str(), unescaped;
                for (size_t i = 0; i < lit.size(); ++i) {
                    if (lit[i] != '\\' || i + 1 == lit.size()) {
                        unescaped += lit[i];
                        continue;
                    }
                    const char e = lit[++i];
                    unescaped += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                }
                out.insert(unescaped);
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("every UI string has a Japanese translation") {
    std::set<std::string> strings = trLiteralsInSources();
    CHECK(strings.size() > 300);

    // Commands (menus, toolbar, shortcut editor).
    {
        ui::AppOptions opt;
        opt.dataDir = test::makeTempDir("i18n");
        opt.headless = true;
        opt.loadSettings = false;
        ui::App app(std::move(opt));
        for (const auto& a : app.commands().actions()) {
            strings.insert(a.label);
            strings.insert(a.category);
        }
    }
    // Effects, transitions and their parameters.
    audio::registerAudioEffects();
    auto addParams = [&](const std::vector<ParamDef>& defs) {
        for (const auto& d : defs) {
            strings.insert(d.label);
            if (!d.group.empty()) strings.insert(d.group);
            for (const auto& e : d.enumLabels) strings.insert(e);
        }
    };
    for (auto kind : {fx::EffectKind::Video, fx::EffectKind::Audio})
        for (const fx::EffectDef* d : fx::EffectRegistry::instance().list(kind)) {
            strings.insert(d->name);
            strings.insert(d->category);
            addParams(d->params);
        }
    for (const auto& t : fx::transitions()) {
        strings.insert(t.name);
        addParams(t.params);
    }
    addParams(transformParamDefs());
    addParams(textParamDefs());
    addParams(audioParamDefs());
    for (const auto& p : exp::presets()) {
        strings.insert(p.name);
        if (!p.description.empty()) strings.insert(p.description);
    }
    for (int i = 0; i <= static_cast<int>(ClipKind::Compound); ++i) strings.insert(clipKindName(static_cast<ClipKind>(i)));
    for (int i = 0; i <= static_cast<int>(TrackKind::Effect); ++i) strings.insert(trackKindName(static_cast<TrackKind>(i)));
    for (int i = 0; i < static_cast<int>(BlendMode::Count); ++i) strings.insert(blendModeName(static_cast<BlendMode>(i)));
    for (int i = 0; i <= static_cast<int>(MarkerKind::Todo); ++i) strings.insert(markerKindName(static_cast<MarkerKind>(i)));
    // Plan targets shown through tr(variable) in ai/plan.cpp.
    for (const char* t : {"music", "voice", "all", "selected", "all_video", "all_audio", "in", "out", "both"}) strings.insert(t);

    std::vector<std::string> missing;
    for (const auto& s : strings)
        if (!s.empty() && !hasTranslation(s)) missing.push_back(s);
    std::string list;
    for (const auto& m : missing) list += "\n  " + m;
    CHECK_MESSAGE(missing.empty(), missing.size(), " untranslated:", list);
}

TEST_CASE("Japanese is the default UI language") {
    AppSettings s;
    CHECK(s.general.language == "ja");
    CHECK(languageFromSetting("ja") == Language::Japanese);
    setLanguage(Language::Japanese);
    CHECK(std::string(tr("Export")) == "書き出し");
    CHECK(std::string(tr("Split at Playhead")) == "再生ヘッドで分割");
    setLanguage(Language::English);
    CHECK(std::string(tr("Export")) == "Export");
}
