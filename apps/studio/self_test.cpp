#include "apps/studio/self_test.h"

#include <chrono>
#include <memory>

#include <nlohmann/json.hpp>

#include "core/file_io.h"
#include "core/log.h"
#include "core/strings.h"
#include "encode/encoder.h"
#include "ui/app.h"
#include "ui/main_window.h"

namespace avc::studio {

namespace {

struct Step {
    std::string name;
    bool ok = false;
    std::string detail;
    double seconds = 0;
};

class SelfTest {
public:
    SelfTest(std::string outDir, std::vector<std::string> media) : out_(std::move(outDir)), media_(std::move(media)) {}

    void operator()(ui::App& app, ui::MainWindow& win, ui::FrameControl& fc) {
        ++frame_;
        const double now = app.timeSec();
        auto elapsed = [&] { return now - stepStart_; };
        auto next = [&](bool ok, std::string detail = {}) {
            results_.push_back({stepName_, ok, std::move(detail), elapsed()});
            AVC_INFO("selftest", "{} {} {}", ok ? "PASS" : "FAIL", stepName_, results_.back().detail);
            ++state_;
            stepStart_ = now;
            stepName_.clear();
        };
        auto begin = [&](const char* name) {
            if (stepName_.empty()) {
                stepName_ = name;
                stepStart_ = now;
            }
        };
        auto timedOut = [&](double s) { return elapsed() > s; };
        const std::string base = out_ + "/";

        switch (state_) {
        case 0:  // let the window and docking layout settle
            begin("startup");
            if (frame_ > 30) {
                fc.screenshotPath = base + "01_startup.png";
                next(app.sequence() != nullptr, app.device().info().name + " / " + app.device().info().adapter);
            }
            break;
        case 1:
            begin("import");
            if (!importStarted_) {
                importStarted_ = true;
                if (media_.empty()) {
                    next(true, "no media given (title-only run)");
                    break;
                }
                app.importFiles(media_);
            } else if (app.importsInProgress() == 0 && app.project().media.size() >= media_.size()) {
                next(true, std::to_string(app.project().media.size()) + " items");
            } else if (timedOut(60)) {
                next(false, "timeout");
            }
            break;
        case 2: {
            begin("timeline edit");
            bool ok = true;
            std::string detail;
            MediaId video = kInvalidId, audioOnly = kInvalidId;
            for (const auto& m : app.project().media) {
                if (m->info.hasVideo() && video == kInvalidId) video = m->id;
                else if (!m->info.hasVideo() && m->info.hasAudio() && audioOnly == kInvalidId) audioOnly = m->id;
            }
            if (video != kInvalidId) {
                app.seek(Time{0});
                app.appendMediaAtPlayhead(video);
                app.seek(Time::fromSeconds(2.0));
                app.selectAll();
                app.splitAtPlayhead(false);
                ok &= app.sequence()->clipCount() >= 4;
                detail += "clips=" + std::to_string(app.sequence()->clipCount());
            }
            if (audioOnly != kInvalidId) {
                const auto aud = app.sequence()->audioTrackIndices();
                ok &= static_cast<bool>(app.addMediaToTimeline(audioOnly, Time::fromSeconds(1.0), -1, aud.size() > 1 ? aud[1] : aud[0], false));
            }
            app.seek(Time::fromSeconds(0.5));
            const ClipId title = app.addTextClip("AviCap Studio セルフテスト");
            ok &= title != kInvalidId;
            app.addSubtitleClip("字幕 / Subtitles OK", Time::fromSeconds(0.5), Time::fromSeconds(2.5));
            // Effects + colour on the first visual media clip.
            ClipId firstVideo = kInvalidId;
            const Sequence* seq = app.sequence();
            for (int ti : seq->visualTrackIndices())
                for (const auto& c : seq->tracks[static_cast<size_t>(ti)]->clips)
                    if (c->kind == ClipKind::Media && firstVideo == kInvalidId) firstVideo = c->id;
            if (firstVideo != kInvalidId) {
                app.selection.clips = {firstVideo};
                app.applyEffect("color.basic");
                app.applyEffect("stylize.vignette");
                app.seek(Time{0});
                app.applyTransition("cross-dissolve", Time::fromSeconds(0.5));
                const Clip* c = app.sequence()->clip(firstVideo);
                ok &= c && c->effects.size() == 2 && c->transitionIn.has_value();
                detail += " effects=" + std::to_string(c ? c->effects.size() : 0);
            }
            app.selection.clips.clear();
            app.ui().showScopes = true;
            app.ui().zoomToFit = true;
            app.seek(Time::fromSeconds(1.0));
            next(ok, detail);
            break;
        }
        case 3:
            begin("preview render");
            if (const auto f = app.preview().latest(); f.texture && f.time.seconds() > 0.9 && f.error.empty()) {
                fc.screenshotPath = base + "02_edit.png";
                char buf[96];
                std::snprintf(buf, sizeof buf, "%dx%d in %.1f ms", f.width, f.height, f.renderMs);
                next(true, buf);
            } else if (timedOut(30)) {
                next(false, f.error.empty() ? "timeout" : f.error);
            }
            break;
        case 4:
            begin("playback");
            if (!playStarted_) {
                playStarted_ = true;
                app.seek(Time{0});
                app.togglePlay();
                playFrom_ = now;
            } else if (now - playFrom_ > 1.5) {
                const double pos = app.playhead().seconds();
                app.togglePlay();
                next(pos > 0.5, "position " + std::to_string(pos) + " s after 1.5 s, device " + app.playback().output().deviceName());
            }
            break;
        case 5:
            begin("save project");
            if (!saveStarted_) {
                saveStarted_ = true;
                app.saveProjectAs(base + "selftest.avicap", [this](bool ok) { saveOk_ = ok ? 1 : 0; });
            } else if (saveOk_ >= 0) {
                next(saveOk_ == 1 && !app.doc().dirty());
            } else if (timedOut(30)) {
                next(false, "timeout");
            }
            break;
        case 6: {
            begin("export");
            if (!job_) {
                exp::ExportSettings s = app.defaultExportSettings();
                s.outputPath = base + "selftest.mp4";
                s.container = "mp4";
                s.videoCodec = enc::VideoCodec::H264;
                if (!enc::EncoderCatalog::instance().best(enc::VideoCodec::H264, true)) s.videoCodec = enc::VideoCodec::MPEG4;
                s.width = 1280;
                s.height = 720;
                s.range = TimeRange{Time{0}, Time::fromSeconds(3.0)};
                job_ = app.queueExport(s);
                if (!job_) next(false, "could not queue");
            } else {
                const auto p = job_->progress();
                if (p.state == exp::ExportState::Done || p.state == exp::ExportState::Failed || p.state == exp::ExportState::Cancelled) {
                    fc.screenshotPath = base + "03_export.png";
                    next(p.state == exp::ExportState::Done && job_->report().ok, p.encoder + " | " + job_->report().summary() + " " + p.message);
                } else if (timedOut(300)) {
                    next(false, "timeout");
                }
            }
            break;
        }
        case 7: {
            // Write the report and exit.
            nlohmann::json j;
            bool all = true;
            for (const auto& r : results_) {
                j["steps"].push_back({{"name", r.name}, {"ok", r.ok}, {"detail", r.detail}, {"seconds", r.seconds}});
                all &= r.ok;
            }
            j["ok"] = all;
            j["device"] = app.device().info().name + " / " + app.device().info().adapter;
            j["encoders"] = enc::EncoderCatalog::instance().summary();
            writeFileAtomic(pathFromUtf8(base + "selftest.json"), j.dump(2));
            AVC_INFO("selftest", "finished: {}", all ? "ALL PASSED" : "FAILURES");
            fc.exitCode = all ? 0 : 1;
            fc.exit = true;
            app.ui().exitConfirmed = true;
            ++state_;
            break;
        }
        default: break;
        }
        (void)win;
    }

private:
    std::string out_;
    std::vector<std::string> media_;
    int frame_ = 0;
    int state_ = 0;
    std::string stepName_;
    double stepStart_ = 0;
    std::vector<Step> results_;
    bool importStarted_ = false;
    bool playStarted_ = false;
    double playFrom_ = 0;
    bool saveStarted_ = false;
    int saveOk_ = -1;
    std::shared_ptr<exp::ExportJob> job_;
};

}  // namespace

ui::FrameDriver makeSelfTest(const std::string& outDir, const std::vector<std::string>& media) {
    auto test = std::make_shared<SelfTest>(outDir, media);
    return [test](ui::App& app, ui::MainWindow& win, ui::FrameControl& fc) { (*test)(app, win, fc); };
}

}  // namespace avc::studio
