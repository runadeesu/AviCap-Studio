// avicap-cli: headless AviCap Studio engine front-end.
//
// Used for automation and CI, and by the desktop app as a crash-isolated
// worker process for long jobs (proxies). Commands:
//   version | probe <file> | encoders | verify <file> [...]
//   render-frame <project.avicap> <seconds> <out.png> [--width N] [--gpu cpu|d3d11]
//   export <project.avicap> <out> [--preset id] [--start s] [--duration s] [--width N --height N]
//          [--codec h264|hevc|av1|prores|dnxhr|mpeg4] [--encoder name] [--bitrate kbps] [--gpu cpu|d3d11]
//   golden-path <video> <audio> <output-dir>
//   proxy <input> <output> [--height N]

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio/dsp.h"
#include "avicap_build_info.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "encode/encoder.h"
#include "export/exporter.h"
#include "media/probe.h"
#include "project/serialize.h"
#include "proxy/proxy.h"
#include "render/compositor.h"
#include "stb_image_write.h"
#include "timeline/document.h"
#include "timeline/edit_ops.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

using namespace avc;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    [[nodiscard]] std::string opt(const std::string& k, const std::string& def = {}) const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
    [[nodiscard]] bool flag(const std::string& k) const { return options.count(k) != 0; }
};

Args parseArgs(const std::vector<std::string>& argv) {
    Args a;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (startsWith(s, "--")) {
            const std::string key = s.substr(2);
            if (i + 1 < argv.size() && !startsWith(argv[i + 1], "--")) a.options[key] = argv[++i];
            else a.options[key] = "1";
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

void out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::unique_ptr<gpu::Device> makeDevice(const std::string& which) {
#if defined(_WIN32)
    if (which != "cpu") {
        std::string err;
        gpu::D3D11DeviceOptions o;
        o.warp = which == "warp";
        o.shaderCacheDir = pathToUtf8(cacheDir() / "Shaders");
        if (auto d = gpu::createD3D11Device(o, &err)) return d;
        std::fprintf(stderr, "Direct3D 11 unavailable (%s); using CPU renderer\n", err.c_str());
    }
#else
    (void)which;
#endif
    return gpu::createCpuDevice();
}

enc::VideoCodec codecFromName(const std::string& n) {
    if (n == "hevc" || n == "h265") return enc::VideoCodec::HEVC;
    if (n == "av1") return enc::VideoCodec::AV1;
    if (n == "prores") return enc::VideoCodec::ProRes;
    if (n == "dnxhr") return enc::VideoCodec::DNxHR;
    if (n == "vp9") return enc::VideoCodec::VP9;
    if (n == "mpeg4") return enc::VideoCodec::MPEG4;
    return enc::VideoCodec::H264;
}

int cmdProbe(const Args& a) {
    if (a.positional.size() < 2) return 2;
    auto m = createMediaItem(a.positional[1]);
    if (!m) {
        std::fprintf(stderr, "probe failed: %s\n", m.errorMessage().c_str());
        return 1;
    }
    out(mediaInfoToJson(m->info).dump(2));
    return 0;
}

int cmdEncoders() {
    nlohmann::json list = nlohmann::json::array();
    for (enc::VideoCodec c : {enc::VideoCodec::H264, enc::VideoCodec::HEVC, enc::VideoCodec::AV1, enc::VideoCodec::ProRes,
                              enc::VideoCodec::DNxHR, enc::VideoCodec::VP9, enc::VideoCodec::MPEG4})
        for (const auto& e : enc::EncoderCatalog::instance().encoders(c))
            list.push_back({{"codec", enc::videoCodecName(c)},
                            {"encoder", e.name},
                            {"type", enc::encoderKindName(e.kind)},
                            {"available", e.available},
                            {"reason", e.reason}});
    out(list.dump(2));
    return 0;
}

int cmdVerify(const Args& a) {
    if (a.positional.size() < 2) return 2;
    exp::VerifyExpectations ex;
    ex.durationSec = std::atof(a.opt("duration", "0").c_str());
    ex.width = std::atoi(a.opt("width", "0").c_str());
    ex.height = std::atoi(a.opt("height", "0").c_str());
    ex.audio = a.flag("audio");
    auto rep = exp::verifyExport(a.positional[1], ex);
    out(rep.summary());
    return rep.ok ? 0 : 1;
}

Result<ProjectPtr> loadProject(const std::string& path) {
    auto r = loadProjectFile(pathFromUtf8(path));
    if (!r) return r.status();
    for (const auto& w : r->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return r->project;
}

int cmdRenderFrame(const Args& a) {
    if (a.positional.size() < 4) return 2;
    auto p = loadProject(a.positional[1]);
    if (!p) {
        std::fprintf(stderr, "%s\n", p.errorMessage().c_str());
        return 1;
    }
    const Sequence* seq = (*p)->active();
    if (!seq) return 1;
    auto dev = makeDevice(a.opt("gpu", "auto"));
    MediaFrameProvider frames;
    Compositor comp(*dev, frames, text::createTextRasterizer());
    const int w = std::atoi(a.opt("width", std::to_string(seq->width)).c_str());
    const int h = static_cast<int>(std::lround(static_cast<double>(w) * seq->height / seq->width));
    auto tex = comp.render(*p, *seq, Time::fromSeconds(std::atof(a.positional[2].c_str())), RenderOptions{w, h});
    if (!tex) {
        std::fprintf(stderr, "render failed: %s\n", tex.errorMessage().c_str());
        return 1;
    }
    auto rgba = gpu::readbackRgba8(*dev, **tex);
    const bool ok = stbi_write_png(a.positional[3].c_str(), w, h, 4, rgba.data(), w * 4) != 0;
    out(std::string(ok ? "wrote " : "failed to write ") + a.positional[3] + " (" + std::to_string(comp.stats().milliseconds) + " ms, " +
        dev->info().name + ")");
    return ok ? 0 : 1;
}

int runExport(const ProjectPtr& project, const exp::ExportSettings& es, const std::string& gpuWhich) {
    const std::string gw = gpuWhich;
    exp::ExportJob job(project, project->activeSequence, es, [gw] { return makeDevice(gw); });
    auto token = CancelToken::create();
    std::thread progress([&] {
        while (true) {
            const auto p = job.progress();
            if (p.state == exp::ExportState::Done || p.state == exp::ExportState::Failed || p.state == exp::ExportState::Cancelled) break;
            std::fprintf(stderr, "\r%-10s %5.1f%%  %lld/%lld frames  %.1f fps  %s   ", exp::exportStateName(p.state),
                         p.fraction() * 100.0, static_cast<long long>(p.framesDone), static_cast<long long>(p.totalFrames),
                         p.fps, p.encoder.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    Status st = job.run(token);
    progress.join();
    std::fprintf(stderr, "\n");
    if (!st) {
        std::fprintf(stderr, "export failed: %s\n", st.message().c_str());
        return 1;
    }
    out("exported " + es.outputPath + " in " + std::to_string(job.progress().elapsedSec) + " s using " + job.progress().encoder);
    if (es.verify) out("verification: " + job.report().summary());
    return 0;
}

int cmdExport(const Args& a) {
    if (a.positional.size() < 3) return 2;
    auto p = loadProject(a.positional[1]);
    if (!p) {
        std::fprintf(stderr, "%s\n", p.errorMessage().c_str());
        return 1;
    }
    exp::ExportSettings es;
    es.outputPath = a.positional[2];
    if (a.flag("preset")) es = exp::applyPreset(a.opt("preset"), es);
    if (a.flag("codec")) es.videoCodec = codecFromName(a.opt("codec"));
    if (a.flag("encoder")) es.encoder = a.opt("encoder");
    if (a.flag("bitrate")) es.bitrateKbps = std::atoi(a.opt("bitrate").c_str());
    if (a.flag("width")) es.width = std::atoi(a.opt("width").c_str());
    if (a.flag("height")) es.height = std::atoi(a.opt("height").c_str());
    if (a.flag("no-hw")) es.preferHardware = false;
    const std::string ext = toLower(pathToUtf8(pathFromUtf8(es.outputPath).extension()));
    if (ext == ".mov") es.container = "mov";
    else if (ext == ".mkv") es.container = "mkv";
    else if (ext == ".webm") es.container = "webm";
    if (a.flag("start") || a.flag("duration")) {
        const Time start = Time::fromSeconds(std::atof(a.opt("start", "0").c_str()));
        const Time dur = a.flag("duration") ? Time::fromSeconds(std::atof(a.opt("duration").c_str()))
                                            : (*p)->active()->duration() - start;
        es.range = TimeRange{start, dur};
    }
    return runExport(*p, es, a.opt("gpu", "auto"));
}

// Self-contained integration test of the golden editing path.
int cmdGoldenPath(const Args& a) {
    if (a.positional.size() < 4) return 2;
    const std::string video = a.positional[1], voice = a.positional[2];
    const auto dir = pathFromUtf8(a.positional[3]);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Document doc(std::make_shared<Project>(makeProject("Golden Path", ProjectSettings{1280, 720, {30, 1}, 48000})));
    const SequenceId sid = doc.project().activeSequence;
    auto vm = createMediaItem(video);
    auto am = createMediaItem(voice);
    if (!vm || !am) {
        std::fprintf(stderr, "import failed\n");
        return 1;
    }
    auto check = [](const Status& st, const char* what) {
        if (!st) std::fprintf(stderr, "%s failed: %s\n", what, st.message().c_str());
        return st.isOk();
    };
    bool ok = check(doc.edit("Import", [&](ProjectEditor& pe) {
        pe.addMedia(*vm);
        pe.addMedia(*am);
        return Status::ok();
    }), "import");
    std::vector<ClipId> av;
    ok = ok && check(doc.editSequence("Add", sid, [&](SequenceEditor& e) {
        auto r = edit::addMediaClip(e, *doc.project().findMedia(vm->id), Time{0}, 0, 3, edit::PlaceMode::Overwrite);
        if (r) av = *r;
        return r.status();
    }), "timeline");
    ok = ok && check(doc.editSequence("Split", sid, [&](SequenceEditor& e) {
        return edit::splitClip(e, av[0], Time::fromSeconds(2)).status();
    }), "split");
    ok = ok && check(doc.editSequence("Title", sid, [&](SequenceEditor& e) {
        Clip t = makeTextClip("AviCap Studio", Time::fromSeconds(0.5), Time::fromSeconds(2.5));
        t.textParams.setStatic("strokeWidth", pv(4));
        return edit::placeClips(e, {{1, t}}, edit::PlaceMode::Overwrite);
    }), "text");
    ok = ok && check(doc.editSequence("Voice", sid, [&](SequenceEditor& e) {
        return edit::addMediaClip(e, *doc.project().findMedia(am->id), Time::fromSeconds(1), -1, 4, edit::PlaceMode::Overwrite).status();
    }), "audio");
    const auto projFile = dir / "golden.avicap";
    ok = ok && check(saveProjectFile(doc.project(), projFile), "save");
    auto loaded = loadProjectFile(projFile);
    if (!ok || !loaded) {
        std::fprintf(stderr, "reload failed\n");
        return 1;
    }
    out("project saved and reloaded: " + pathToUtf8(projFile));
    exp::ExportSettings es;
    es.outputPath = pathToUtf8(dir / "golden.mp4");
    es.width = 1280;
    es.height = 720;
    es.bitrateKbps = 6000;
    es.range = TimeRange{Time{0}, Time::fromSeconds(4)};
    if (!enc::EncoderCatalog::instance().best(enc::VideoCodec::H264)) es.videoCodec = enc::VideoCodec::MPEG4;
    return runExport(loaded->project, es, a.opt("gpu", "auto"));
}

int cmdProxy(const Args& a) {
    if (a.positional.size() < 3) return 2;
    proxy::ProxyRequest req;
    req.source = a.positional[1];
    req.output = a.positional[2];
    req.height = std::atoi(a.opt("height", "540").c_str());
    auto token = CancelToken::create();
    Status st = proxy::generateProxy(req, token, [](float f) {
        std::fprintf(stderr, "\rproxy %5.1f%%", f * 100.0f);
        std::fprintf(stdout, "progress %.4f\n", f);
        std::fflush(stdout);
    });
    std::fprintf(stderr, "\n");
    if (!st) {
        std::fprintf(stderr, "proxy failed: %s\n", st.message().c_str());
        return 1;
    }
    out("proxy written: " + req.output);
    return 0;
}

void usage() {
    out("AviCap Studio CLI " AVICAP_VERSION_STRING
        "\nUsage:\n"
        "  avicap-cli version\n"
        "  avicap-cli probe <media>\n"
        "  avicap-cli encoders\n"
        "  avicap-cli render-frame <project.avicap> <seconds> <out.png> [--width N] [--gpu auto|cpu|warp]\n"
        "  avicap-cli export <project.avicap> <out.mp4|.mov|.mkv> [--preset id] [--codec h264|hevc|av1|prores|dnxhr]\n"
        "                    [--encoder name] [--bitrate kbps] [--width N --height N] [--start s] [--duration s] [--gpu ...]\n"
        "  avicap-cli verify <file> [--duration s] [--width N --height N] [--audio]\n"
        "  avicap-cli proxy <media> <out.mp4> [--height 540]\n"
        "  avicap-cli golden-path <video> <audio> <output-dir>");
}

}  // namespace

int runCli(const std::vector<std::string>& argv) {
    LogConfig lc;
    lc.directory = logsDir();
    lc.toStderr = false;
    lc.minLevel = LogLevel::Info;
    const Args a = parseArgs(argv);
    if (a.flag("verbose")) {
        lc.toStderr = true;
        lc.minLevel = LogLevel::Debug;
    }
    log::init(lc);
    audio::registerAudioEffects();
    if (a.positional.empty()) {
        usage();
        return 2;
    }
    const std::string cmd = a.positional[0];
    int rc = 2;
    if (cmd == "version") {
        out("AviCap Studio " AVICAP_VERSION_STRING " (" AVICAP_BUILD_DATE ", " AVICAP_COMPILER ")");
        out("OS: " + osDescription());
        out("CPU: " + cpuDescription());
        rc = 0;
    } else if (cmd == "probe") {
        rc = cmdProbe(a);
    } else if (cmd == "encoders") {
        rc = cmdEncoders();
    } else if (cmd == "verify") {
        rc = cmdVerify(a);
    } else if (cmd == "render-frame") {
        rc = cmdRenderFrame(a);
    } else if (cmd == "export") {
        rc = cmdExport(a);
    } else if (cmd == "golden-path") {
        rc = cmdGoldenPath(a);
    } else if (cmd == "proxy") {
        rc = cmdProxy(a);
    }
    if (rc == 2) usage();
    Jobs::shutdown();
    log::shutdown();
    return rc;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(wideToUtf8(wargv[i]));
    return runCli(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return runCli(args);
}
#endif
