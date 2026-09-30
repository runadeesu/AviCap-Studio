#include "export/exporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "audio/mixer.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "encode/encoder.h"
#include "render/compositor.h"
#include "text/text_raster.h"

namespace avc::exp {

using gpu::Kernel;
using gpu::KernelParams;
using gpu::TexFormat;
using gpu::TexturePtr;

const char* exportStateName(ExportState s) {
    switch (s) {
    case ExportState::Queued: return "Queued";
    case ExportState::Preparing: return "Preparing";
    case ExportState::Rendering: return "Rendering";
    case ExportState::Finalizing: return "Finalizing";
    case ExportState::Verifying: return "Verifying";
    case ExportState::Done: return "Done";
    case ExportState::Failed: return "Failed";
    case ExportState::Cancelled: return "Cancelled";
    case ExportState::Paused: return "Paused";
    }
    return "?";
}

DeviceFactory defaultDeviceFactory() {
    return []() -> std::unique_ptr<gpu::Device> {
#if defined(_WIN32)
        std::string err;
        gpu::D3D11DeviceOptions o;
        o.shaderCacheDir = pathToUtf8(cacheDir() / "Shaders");
        if (auto d = gpu::createD3D11Device(o, &err)) return d;
        AVC_WARN("export", "Direct3D 11 unavailable for export ({}); using CPU renderer", err);
#endif
        return gpu::createCpuDevice();
    };
}

ExportJob::ExportJob(ProjectPtr project, SequenceId sequence, ExportSettings settings, DeviceFactory devices)
    : id_(newId()), project_(std::move(project)), sequence_(sequence), settings_(std::move(settings)), devices_(std::move(devices)) {
    name_ = pathToUtf8(pathFromUtf8(settings_.outputPath).filename());
}

void ExportJob::setState(ExportState s, std::string msg) {
    std::lock_guard lock(mutex_);
    progress_.state = s;
    if (!msg.empty()) progress_.message = std::move(msg);
}

ExportProgress ExportJob::progress() const {
    std::lock_guard lock(mutex_);
    ExportProgress p = progress_;
    if (paused_ && (p.state == ExportState::Rendering)) p.state = ExportState::Paused;
    return p;
}

void ExportJob::pause() { paused_ = true; }

void ExportJob::resume() {
    paused_ = false;
    pauseCv_.notify_all();
}

void ExportJob::resetForRetry() {
    std::lock_guard lock(mutex_);
    progress_ = {};
    report_ = {};
    paused_ = false;
}

namespace {

// GPU conversion of the final frame to the encoder's plane layout + readback.
class PlaneConverter {
public:
    PlaneConverter(gpu::Device& dev, int w, int h, enc::PlaneLayout layout) : dev_(dev), w_(w), h_(h), layout_(layout) {
        const bool wide = layout == enc::PlaneLayout::P010 || layout == enc::PlaneLayout::YUV422P10 ||
                          layout == enc::PlaneLayout::YUV420P10;
        const bool semi = layout == enc::PlaneLayout::NV12 || layout == enc::PlaneLayout::P010;
        const bool full422 = layout == enc::PlaneLayout::YUV422P || layout == enc::PlaneLayout::YUV422P10;
        const int cw = (w + 1) / 2, chh = full422 ? h : (h + 1) / 2;
        const TexFormat one = wide ? TexFormat::R16 : TexFormat::R8;
        const TexFormat two = wide ? TexFormat::RG16 : TexFormat::RG8;
        // 10-bit planar formats store values in the low bits.
        scale_ = (layout == enc::PlaneLayout::YUV422P10 || layout == enc::PlaneLayout::YUV420P10) ? 1023.0f / 65535.0f : 1.0f;
        planes_.push_back({dev.createTexture({w, h, one, true}), 0, w, h, wide ? 2 : 1});
        if (semi) {
            planes_.push_back({dev.createTexture({cw, chh, two, true}), 3, cw, chh, wide ? 4 : 2});
        } else {
            planes_.push_back({dev.createTexture({cw, chh, one, true}), 1, cw, chh, wide ? 2 : 1});
            planes_.push_back({dev.createTexture({cw, chh, one, true}), 2, cw, chh, wide ? 2 : 1});
        }
        for (auto& p : planes_) p.buffer.resize(static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * static_cast<size_t>(p.bpp));
    }

    enc::EncodeFrame convert(const gpu::Texture& frame, int64_t index) {
        enc::EncodeFrame f;
        f.index = index;
        for (size_t i = 0; i < planes_.size(); ++i) {
            Plane& p = planes_[i];
            KernelParams k;
            k.set(0, static_cast<float>(p.plane), 0.0f, scale_);
            k.set(1, 0.2126f, 0.7152f, 0.0722f);
            dev_.run(Kernel::RgbToYuv, k, {&frame}, *p.tex);
            dev_.readback(*p.tex, p.buffer.data(), p.w * p.bpp);
            f.planes[i] = p.buffer.data();
            f.strides[i] = p.w * p.bpp;
        }
        return f;
    }

private:
    struct Plane {
        TexturePtr tex;
        int plane;
        int w, h, bpp;
        std::vector<uint8_t> buffer;
    };
    gpu::Device& dev_;
    int w_, h_;
    enc::PlaneLayout layout_;
    float scale_ = 1.0f;
    std::vector<Plane> planes_;
};

}  // namespace

Status ExportJob::run(const CancelToken& cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    setState(ExportState::Preparing, "Preparing");
    const Sequence* seq = project_ ? project_->findSequence(sequence_) : nullptr;
    auto fail = [&](const std::string& msg) {
        setState(ExportState::Failed, msg);
        AVC_ERROR("export", "Export '{}' failed: {}", name_, msg);
        return Status::error(msg);
    };
    if (!seq) return fail("Sequence not found");
    if (settings_.outputPath.empty()) return fail("No output file");

    // ---- resolve settings
    const int W = (settings_.width > 0 ? settings_.width : seq->width) & ~1;
    const int H = (settings_.height > 0 ? settings_.height : seq->height) & ~1;
    const Rational fps = settings_.frameRate.num > 0 ? settings_.frameRate.reduced() : seq->frameRate;
    TimeRange range = settings_.range ? *settings_.range : (seq->workArea ? *seq->workArea : TimeRange{Time{0}, seq->duration()});
    if (range.duration.ticks <= 0) return fail("Nothing to export (the timeline is empty)");
    const int64_t totalFrames = std::max<int64_t>(1, range.duration.toFrames(fps, Rounding::Nearest));
    {
        std::lock_guard lock(mutex_);
        progress_.totalFrames = totalFrames;
    }
    std::string encoderName = settings_.encoder;
    if (encoderName.empty() || encoderName == "auto") {
        const enc::EncoderInfo* best = enc::EncoderCatalog::instance().best(settings_.videoCodec, settings_.preferHardware);
        if (!best) return fail(std::string("No encoder available for ") + enc::videoCodecName(settings_.videoCodec));
        encoderName = best->name;
    }

    // ---- disk space
    const auto outPath = pathFromUtf8(settings_.outputPath);
    const uint64_t estimate = estimateOutputBytes(settings_, W, H, fps.toDouble(), range.duration.seconds());
    if (auto space = queryDiskSpace(outPath.parent_path()); space && space->available < estimate)
        return fail("Not enough disk space: about " + formatBytes(estimate) + " needed, " + formatBytes(space->available) +
                    " available");

    // Render into "<name>.partial.<ext>" and rename on success.
    auto partial = outPath.parent_path() / (outPath.stem().u8string() + u8".partial" + outPath.extension().u8string());
    const std::string partialUtf8 = pathToUtf8(partial);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);

    auto muxer = enc::Muxer::create(partialUtf8, settings_.container == "mkv" ? "matroska" : settings_.container);
    if (!muxer) return fail(muxer.errorMessage());
    enc::VideoEncodeSettings vs;
    vs.encoder = encoderName;
    vs.codec = settings_.videoCodec;
    vs.width = W;
    vs.height = H;
    vs.frameRate = fps;
    vs.rateControl = settings_.rateControl;
    vs.bitrateKbps = settings_.bitrateKbps;
    vs.maxBitrateKbps = settings_.maxBitrateKbps;
    vs.quality = settings_.quality;
    vs.profile = settings_.profile;
    vs.bitDepth = settings_.bitDepth;
    vs.gopFrames = settings_.gopFrames;
    std::string used;
    if (auto r = (*muxer)->addVideo(vs, &used); !r) return fail(r.errorMessage());
    const bool withAudio = settings_.includeAudio && seq->countTracks(TrackKind::Audio) > 0;
    if (withAudio) {
        enc::AudioEncodeSettings as;
        as.codec = settings_.audioCodec;
        as.sampleRate = settings_.sampleRate;
        as.bitrateKbps = settings_.audioBitrateKbps;
        if (auto r = (*muxer)->addAudio(as); !r) return fail(r.errorMessage());
    }
    if (auto st = (*muxer)->start(); !st) return fail(st.message());
    {
        std::lock_guard lock(mutex_);
        progress_.encoder = used;
    }
    AVC_INFO("export", "Export '{}': {}x{} @ {} fps, {} frames, encoder {}", name_, W, H, fps.toString(), totalFrames, used);

    // ---- rendering resources (private to this job)
    std::unique_ptr<gpu::Device> device = devices_();
    FrameProviderOptions fpo;
    fpo.cacheBytes = size_t(256) << 20;
    MediaFrameProvider frames(fpo);
    auto text = text::createTextRasterizer();
    Compositor compositor(*device, frames, text);
    PlaneConverter converter(*device, W, H, (*muxer)->videoLayout());
    audio::AudioSourcePool audioSources(settings_.sampleRate);
    audio::SequenceMixer mixer(audioSources);

    // Letterbox when the output aspect differs from the sequence.
    const double seqAspect = static_cast<double>(seq->width) / seq->height;
    const double outAspect = static_cast<double>(W) / H;
    int fw = W, fh = H;
    if (std::fabs(seqAspect - outAspect) > 0.001) {
        if (seqAspect > outAspect) fh = std::max(2, static_cast<int>(std::lround(W / seqAspect)));
        else fw = std::max(2, static_cast<int>(std::lround(H * seqAspect)));
    }
    TexturePtr letterbox = (fw != W || fh != H) ? device->createTexture({W, H, TexFormat::RGBA16F, true}) : nullptr;

    setState(ExportState::Rendering, "Rendering");
    std::vector<float> audioBuf;
    Status status = Status::ok();
    for (int64_t i = 0; i < totalFrames; ++i) {
        if (paused_) {
            std::unique_lock lk(pauseMutex_);
            pauseCv_.wait(lk, [&] { return !paused_ || cancel.cancelled(); });
        }
        if (cancel.cancelled()) {
            status = Status::error("cancelled");
            break;
        }
        const Time t = range.start + Time::fromFrames(i, fps);
        auto tex = compositor.render(project_, *seq, t, RenderOptions{fw, fh, true, true}, cancel);
        if (!tex) {
            status = Status::error("Render failed at frame " + std::to_string(i) + ": " + tex.errorMessage());
            break;
        }
        const gpu::Texture* finalTex = tex->get();
        if (letterbox) {
            KernelParams cp;
            const float ox = static_cast<float>(W - fw) * 0.5f, oy = static_cast<float>(H - fh) * 0.5f;
            cp.set(0, static_cast<float>(W) / static_cast<float>(fw), static_cast<float>(H) / static_cast<float>(fh),
                   -ox / static_cast<float>(fw), -oy / static_cast<float>(fh));
            cp.set(1, 1.0f);
            device->run(Kernel::Copy, cp, {finalTex}, *letterbox);
            finalTex = letterbox.get();
        }
        const enc::EncodeFrame ef = converter.convert(*finalTex, i);
        if (auto st = (*muxer)->writeVideo(&ef); !st) {
            status = st;
            break;
        }
        if (withAudio) {
            const Time next = range.start + Time::fromFrames(i + 1, fps);
            const int64_t s0 = t.toSamples(settings_.sampleRate, Rounding::Nearest);
            const int64_t s1 = next.toSamples(settings_.sampleRate, Rounding::Nearest);
            const int n = static_cast<int>(s1 - s0);
            audioBuf.resize(static_cast<size_t>(n) * 2);
            mixer.render(project_, *seq, s0, n, audioBuf.data(), cancel);
            if (auto st = (*muxer)->writeAudio(audioBuf.data(), n); !st) {
                status = st;
                break;
            }
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard lock(mutex_);
        progress_.framesDone = i + 1;
        progress_.elapsedSec = elapsed;
        progress_.fps = elapsed > 0 ? static_cast<double>(i + 1) / elapsed : 0;
        progress_.speed = progress_.fps / std::max(1e-6, fps.toDouble());
        progress_.etaSec = progress_.fps > 0 ? static_cast<double>(totalFrames - i - 1) / progress_.fps : 0;
        progress_.bytes = (*muxer)->bytesWritten();
    }
    if (!status) {
        (*muxer)->abort();
        std::filesystem::remove(partial, ec);
        if (cancel.cancelled()) {
            setState(ExportState::Cancelled, "Cancelled");
            AVC_INFO("export", "Export '{}' cancelled", name_);
            return Status::error("cancelled");
        }
        return fail(status.message());
    }
    setState(ExportState::Finalizing, "Finalizing");
    if (auto st = (*muxer)->finish(); !st) {
        std::filesystem::remove(partial, ec);
        return fail(st.message());
    }
    std::filesystem::remove(outPath, ec);
    std::filesystem::rename(partial, outPath, ec);
    if (ec) return fail("Cannot move output into place: " + ec.message());

    if (settings_.verify) {
        setState(ExportState::Verifying, "Verifying");
        VerifyExpectations ex;
        ex.durationSec = static_cast<double>(totalFrames) / fps.toDouble();
        ex.width = W;
        ex.height = H;
        ex.frameRate = fps;
        ex.audio = withAudio;
        VerifyReport rep = verifyExport(settings_.outputPath, ex, cancel);
        {
            std::lock_guard lock(mutex_);
            report_ = rep;
            progress_.bytes = rep.fileSize;
        }
        AVC_INFO("export", "Verification of '{}': {}", name_, rep.summary());
        if (!rep.ok) return fail("Verification failed: " + (rep.problems.empty() ? std::string() : rep.problems.front()));
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard lock(mutex_);
        progress_.elapsedSec = elapsed;
        progress_.etaSec = 0;
    }
    setState(ExportState::Done, "Done");
    AVC_INFO("export", "Export '{}' finished in {:.1f} s", name_, elapsed);
    return Status::ok();
}

// ============================================================ ExportQueue

ExportQueue::ExportQueue() {
    thread_ = std::thread([this] { worker(); });
}

ExportQueue::~ExportQueue() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        if (current_) {
            currentCancel_.cancel();
            current_->resume();
        }
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

Id ExportQueue::add(std::shared_ptr<ExportJob> job) {
    const Id id = job->id();
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
    return id;
}

bool ExportQueue::remove(Id id) {
    std::lock_guard lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if ((*it)->id() != id) continue;
        if (*it == current_) return false;
        jobs_.erase(it);
        return true;
    }
    return false;
}

bool ExportQueue::moveUp(Id id) {
    std::lock_guard lock(mutex_);
    for (size_t i = 1; i < jobs_.size(); ++i)
        if (jobs_[i]->id() == id) {
            std::swap(jobs_[i], jobs_[i - 1]);
            return true;
        }
    return false;
}

bool ExportQueue::moveDown(Id id) {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i + 1 < jobs_.size(); ++i)
        if (jobs_[i]->id() == id) {
            std::swap(jobs_[i], jobs_[i + 1]);
            return true;
        }
    return false;
}

void ExportQueue::cancel(Id id) {
    std::lock_guard lock(mutex_);
    if (current_ && current_->id() == id) {
        currentCancel_.cancel();
        current_->resume();
        return;
    }
    for (auto& j : jobs_)
        if (j->id() == id && j->progress().state == ExportState::Queued) j->markCancelled();
}

void ExportQueue::pause(Id id) {
    std::lock_guard lock(mutex_);
    for (auto& j : jobs_)
        if (j->id() == id) j->pause();
}

void ExportQueue::resume(Id id) {
    std::lock_guard lock(mutex_);
    for (auto& j : jobs_)
        if (j->id() == id) j->resume();
}

bool ExportQueue::retry(Id id) {
    std::lock_guard lock(mutex_);
    for (auto& j : jobs_) {
        if (j->id() != id) continue;
        const ExportState s = j->progress().state;
        if (s != ExportState::Failed && s != ExportState::Cancelled && s != ExportState::Done) return false;
        j->resetForRetry();
        cv_.notify_all();
        return true;
    }
    return false;
}

std::vector<std::shared_ptr<ExportJob>> ExportQueue::jobs() const {
    std::lock_guard lock(mutex_);
    return jobs_;
}

bool ExportQueue::busy() const {
    std::lock_guard lock(mutex_);
    if (current_) return true;
    for (auto& j : jobs_)
        if (j->progress().state == ExportState::Queued) return true;
    return false;
}

void ExportQueue::cancelAll() {
    std::lock_guard lock(mutex_);
    if (current_) {
        currentCancel_.cancel();
        current_->resume();
    }
}

void ExportQueue::setOnFinished(std::function<void(const ExportJob&)> fn) {
    std::lock_guard lock(mutex_);
    onFinished_ = std::move(fn);
}

void ExportQueue::worker() {
    setCurrentThreadName("export");
    for (;;) {
        std::shared_ptr<ExportJob> job;
        CancelToken token;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                if (stop_) return true;
                for (auto& j : jobs_)
                    if (j->progress().state == ExportState::Queued) return true;
                return false;
            });
            if (stop_) return;
            for (auto& j : jobs_)
                if (j->progress().state == ExportState::Queued) {
                    job = j;
                    break;
                }
            current_ = job;
            currentCancel_ = CancelToken::create();
            token = currentCancel_;
        }
        job->run(token);
        std::function<void(const ExportJob&)> cb;
        {
            std::lock_guard lock(mutex_);
            current_.reset();
            cb = onFinished_;
        }
        if (cb) cb(*job);
    }
}

}  // namespace avc::exp
