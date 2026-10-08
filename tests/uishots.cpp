// avicap_uishots: renders the editor headless and saves PNG screenshots of
// the main screens (design review, documentation). No GPU or window needed.
//
//   avicap_uishots <output-folder> [width height]

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

#include "core/i18n.h"
#include "core/strings.h"
#include "tests/test_support.h"
#include "ui/app.h"
#include "ui/headless.h"

using namespace avc;
using namespace avc::ui;

namespace {

bool runUntil(HeadlessHost& host, const std::function<bool()>& cond, double timeoutSec) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(timeoutSec * 1000));
    while (std::chrono::steady_clock::now() < end) {
        host.frame();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: avicap_uishots <output-folder> [width height]\n");
        return 2;
    }
    const std::filesystem::path out = pathFromUtf8(argv[1]);
    const float w = argc >= 4 ? static_cast<float>(std::atof(argv[2])) : 1600.0f;
    const float h = argc >= 4 ? static_cast<float>(std::atof(argv[3])) : 900.0f;
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    test::initTestEnvironment();
    setLanguage(Language::Japanese);
    int failures = 0;
    {
        AppOptions opt;
        opt.dataDir = test::makeTempDir("uishots");
        opt.headless = true;
        opt.loadSettings = false;
        App app(std::move(opt));
        HeadlessHost host(app, ImVec2(w, h));
        auto shot = [&](const char* name, int frames = 4) {
            host.frames(frames);
            const std::string p = pathToUtf8(out / name);
            if (host.saveScreenshot(p)) std::printf("wrote %s\n", p.c_str());
            else {
                std::printf("FAILED %s\n", p.c_str());
                ++failures;
            }
        };
        shot("01_welcome.png", 6);

        const std::string video = test::testMedia("av_1080p30.mp4");
        const std::string music = test::testMedia("beats_120bpm.wav");
        const std::string voice = test::testMedia("speech_silence.wav");
        if (video.empty()) {
            std::fprintf(stderr, "test media missing (run the test_media target)\n");
            return 1;
        }
        app.importFiles({video, voice});
        runUntil(host, [&] { return app.project().media.size() == 2 && app.importsInProgress() == 0; }, 30);
        for (const auto& m : app.project().media)
            if (m->info.hasVideo()) app.appendMediaAtPlayhead(m->id);
        app.seek(Time::fromSeconds(1.0));
        app.addTextClip("AviCap Studio");
        app.addSubtitleClip("こんにちは、世界", Time::fromSeconds(0.5), Time::fromSeconds(2.5));
        if (!music.empty()) app.addSoundFiles({music}, true, Time::fromSeconds(0));
        runUntil(host, [&] { return app.importsInProgress() == 0 && app.project().media.size() >= 3; }, 30);
        app.selection.clear();
        // Let thumbnails, waveforms and the preview frame arrive.
        runUntil(host, [&] { return app.preview().latest().texture != nullptr && !app.animating(); }, 40);
        host.frames(10);
        shot("02_edit.png");
        for (const char* page : {"audio", "text", "effects", "transitions", "ai"}) {
            app.openLibrary(page);
            shot((std::string("04_library_") + page + ".png").c_str(), 6);
        }
        app.openLibrary("media");
        // A selected clip shows its properties in the inspector.
        for (const auto& t : app.sequence()->tracks)
            if (t->kind == TrackKind::Video && !t->clips.empty()) {
                app.selectClip(t->clips.front()->id, false, false);
                break;
            }
        shot("05_selected.png", 6);

        for (const char* ws : {"color", "audio", "export"}) {
            app.setWorkspace(ws);
            runUntil(host, [&] { return !app.animating(); }, 10);
            shot((std::string("03_workspace_") + ws + ".png").c_str(), 8);
        }
        app.setWorkspace("edit");
        host.frames(4);
    }
    test::shutdownTestEnvironment();
    return failures == 0 ? 0 : 1;
}
