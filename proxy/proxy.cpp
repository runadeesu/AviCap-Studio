#include "proxy/proxy.h"

#include <algorithm>
#include <cstring>

#include "core/file_io.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/process.h"
#include "core/strings.h"
#include "media/ffmpeg_util.h"

namespace avc::proxy {

int presetHeight(const std::string& preset) {
    const int h = std::atoi(preset.c_str());
    return h >= 144 && h <= 2160 ? h : 540;
}

Status generateProxy(const ProxyRequest& req, const CancelToken& cancel, const std::function<void(float)>& progress) {
    std::string err;
    ff::FormatCtxPtr in = ff::openInput(req.source, &err);
    if (!in) return Status::error("Cannot open source: " + err);
    const AVCodec* dcodec = nullptr;
    const int vi = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &dcodec, 0);
    if (vi < 0 || !dcodec) return Status::error("No video stream");
    AVStream* ist = in->streams[vi];
    for (unsigned i = 0; i < in->nb_streams; ++i)
        if (static_cast<int>(i) != vi) in->streams[i]->discard = AVDISCARD_ALL;
    ff::CodecCtxPtr dec(avcodec_alloc_context3(dcodec));
    avcodec_parameters_to_context(dec.get(), ist->codecpar);
    dec->pkt_timebase = ist->time_base;
    dec->thread_count = 0;
    if (avcodec_open2(dec.get(), dcodec, nullptr) < 0) return Status::error("Cannot open decoder");

    const int srcW = ist->codecpar->width, srcH = ist->codecpar->height;
    int outH = std::min(req.height, srcH) & ~1;
    int outW = std::max(2, static_cast<int>(std::lround(static_cast<double>(srcW) * outH / std::max(1, srcH)))) & ~1;
    if (outH < 2) outH = 2;

    const auto outPath = pathFromUtf8(req.output);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    auto tmp = outPath;
    tmp += ".partial.mov";
    const std::string tmpUtf8 = pathToUtf8(tmp);
    AVFormatContext* ofmt = nullptr;
    if (avformat_alloc_output_context2(&ofmt, nullptr, "mov", tmpUtf8.c_str()) < 0) return Status::error("Cannot create output");
    const AVCodec* ecodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    ff::CodecCtxPtr enc(avcodec_alloc_context3(ecodec));
    enc->width = outW;
    enc->height = outH;
    enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc->time_base = ist->time_base;
    enc->framerate = av_guess_frame_rate(in.get(), ist, nullptr);
    enc->flags |= AV_CODEC_FLAG_QSCALE;
    enc->global_quality = FF_QP2LAMBDA * std::clamp(req.quality, 2, 31);
    enc->color_range = AVCOL_RANGE_JPEG;
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc.get(), ecodec, nullptr) < 0) {
        avformat_free_context(ofmt);
        return Status::error("Cannot open MJPEG encoder");
    }
    AVStream* ost = avformat_new_stream(ofmt, nullptr);
    avcodec_parameters_from_context(ost->codecpar, enc.get());
    ost->time_base = ist->time_base;
    // Preserve display rotation so proxies look like the original.
    if (const AVPacketSideData* sd = av_packet_side_data_get(ist->codecpar->coded_side_data, ist->codecpar->nb_coded_side_data,
                                                             AV_PKT_DATA_DISPLAYMATRIX)) {
        if (AVPacketSideData* nsd = av_packet_side_data_new(&ost->codecpar->coded_side_data, &ost->codecpar->nb_coded_side_data,
                                                            AV_PKT_DATA_DISPLAYMATRIX, sd->size, 0))
            std::memcpy(nsd->data, sd->data, sd->size);
    }
    auto cleanup = [&](bool removeFile) {
        if (ofmt) {
            if (ofmt->pb) avio_closep(&ofmt->pb);
            avformat_free_context(ofmt);
            ofmt = nullptr;
        }
        if (removeFile) std::filesystem::remove(tmp, ec);
    };
    if (avio_open(&ofmt->pb, tmpUtf8.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(ofmt, nullptr) < 0) {
        cleanup(true);
        return Status::error("Cannot write proxy file");
    }
    const int64_t origin = ff::containerOriginUs(in.get());
    const int64_t originTs = av_rescale_q(origin, AVRational{1, AV_TIME_BASE}, ist->time_base);
    const double totalSec = in->duration > 0 ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;

    ff::SwsPtr sws;
    ff::PacketPtr pkt = ff::makePacket(), opkt = ff::makePacket();
    ff::FramePtr frame = ff::makeFrame(), scaled = ff::makeFrame();
    scaled->format = AV_PIX_FMT_YUVJ420P;
    scaled->width = outW;
    scaled->height = outH;
    av_frame_get_buffer(scaled.get(), 32);
    int64_t lastPts = AV_NOPTS_VALUE;
    auto encodeFrame = [&](AVFrame* f) -> bool {
        if (avcodec_send_frame(enc.get(), f) < 0) return false;
        while (avcodec_receive_packet(enc.get(), opkt.get()) == 0) {
            opkt->stream_index = 0;
            av_packet_rescale_ts(opkt.get(), enc->time_base, ofmt->streams[0]->time_base);
            if (av_interleaved_write_frame(ofmt, opkt.get()) < 0) return false;
        }
        return true;
    };
    auto handleDecoded = [&]() -> bool {
        while (avcodec_receive_frame(dec.get(), frame.get()) == 0) {
            int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = lastPts == AV_NOPTS_VALUE ? 0 : lastPts + 1;
            pts -= originTs;
            if (lastPts != AV_NOPTS_VALUE && pts <= lastPts) pts = lastPts + 1;  // strictly increasing
            lastPts = pts;
            SwsContext* c = sws_getCachedContext(sws.release(), frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                 outW, outH, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws.reset(c);
            if (!c) return false;
            av_frame_make_writable(scaled.get());
            sws_scale(c, frame->data, frame->linesize, 0, frame->height, scaled->data, scaled->linesize);
            scaled->pts = pts;
            av_frame_unref(frame.get());
            if (!encodeFrame(scaled.get())) return false;
            if (progress && totalSec > 0)
                progress(static_cast<float>(std::clamp(static_cast<double>(pts) * av_q2d(ist->time_base) / totalSec, 0.0, 1.0)));
        }
        return true;
    };
    bool ok = true;
    while (ok && av_read_frame(in.get(), pkt.get()) >= 0) {
        if (cancel.cancelled()) {
            av_packet_unref(pkt.get());
            cleanup(true);
            return Status::error("cancelled");
        }
        if (pkt->stream_index == vi) {
            avcodec_send_packet(dec.get(), pkt.get());
            ok = handleDecoded();
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(dec.get(), nullptr);
    ok = ok && handleDecoded();
    ok = ok && encodeFrame(nullptr);
    if (!ok || av_write_trailer(ofmt) < 0) {
        cleanup(true);
        return Status::error("Proxy encoding failed");
    }
    cleanup(false);
    std::filesystem::remove(outPath, ec);
    std::filesystem::rename(tmp, outPath, ec);
    if (ec) return Status::error("Cannot move proxy into place: " + ec.message());
    if (progress) progress(1.0f);
    return Status::ok();
}

// ============================================================ ProxyManager

ProxyManager::ProxyManager(std::filesystem::path directory) : dir_(std::move(directory)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
#if defined(_WIN32)
    auto exe = executableDir() / "avicap-cli.exe";
#else
    auto exe = executableDir() / "avicap-cli";
#endif
    if (std::filesystem::exists(exe, ec)) workerExe_ = pathToUtf8(exe);
}

ProxyManager::~ProxyManager() { cancelAll(); }

std::filesystem::path ProxyManager::fileFor(const MediaItem& m, int height) const {
    // Identity: path + size + modification time + proxy height.
    std::string key = m.path + "|" + std::to_string(m.fileSize) + "|" + std::to_string(m.fileModifiedNs) + "|" + std::to_string(height);
    return dir_ / (hex64(fnv1a64(key)) + "_" + std::to_string(height) + "p.mov");
}

std::string ProxyManager::proxyPath(const MediaItem& m, int height) const {
    if (m.info.kind != MediaKind::Video) return {};
    const auto f = fileFor(m, height);
    std::error_code ec;
    return std::filesystem::exists(f, ec) ? pathToUtf8(f) : std::string();
}

void ProxyManager::request(const MediaItem& m, int height) {
    if (m.info.kind != MediaKind::Video) return;
    if (!proxyPath(m, height).empty()) {
        std::lock_guard lock(mutex_);
        status_[m.id] = {ProxyState::Ready, 1.0f, proxyPath(m, height), {}};
        return;
    }
    std::lock_guard lock(mutex_);
    auto it = status_.find(m.id);
    if (it != status_.end() && (it->second.state == ProxyState::Queued || it->second.state == ProxyState::Running)) return;
    status_[m.id] = {ProxyState::Queued, 0.0f, {}, {}};
    const std::string src = m.path;
    const std::string out = pathToUtf8(fileFor(m, height));
    const MediaId id = m.id;
    const std::string worker = workerExe_;
    jobs_[id] = Jobs::cache().submit("Proxy " + m.name, JobPriority::Low, [this, src, out, id, height, worker](JobContext& ctx) {
        {
            std::lock_guard l(mutex_);
            status_[id].state = ProxyState::Running;
        }
        auto setProgress = [this, id, &ctx](float f) {
            ctx.setProgress(f);
            std::lock_guard l(mutex_);
            status_[id].progress = f;
        };
        Status st;
        if (!worker.empty()) {
            auto r = runProcess(worker, {"proxy", src, out, "--height", std::to_string(height)},
                                [&](const std::string& line) {
                                    if (startsWith(line, "progress ")) setProgress(std::strtof(line.c_str() + 9, nullptr));
                                },
                                ctx.token());
            if (!r) st = r.status();
            else if (r->cancelled) st = Status::error("cancelled");
            else if (r->exitCode != 0) st = Status::error("Proxy worker failed (exit code " + std::to_string(r->exitCode) + ")");
        } else {
            ProxyRequest req{src, out, height};
            st = generateProxy(req, ctx.token(), setProgress);
        }
        std::lock_guard l(mutex_);
        if (st) {
            status_[id] = {ProxyState::Ready, 1.0f, out, {}};
            AVC_INFO("proxy", "Proxy ready: {}", out);
        } else {
            status_[id] = {ProxyState::Failed, 0.0f, {}, st.message()};
            AVC_WARN("proxy", "Proxy failed for {}: {}", src, st.message());
        }
    }, group_);
}

void ProxyManager::cancelAll() {
    group_.cancel();
    std::map<MediaId, JobHandle> jobs;
    {
        std::lock_guard lock(mutex_);
        jobs.swap(jobs_);
    }
    for (auto& [id, h] : jobs) h.wait();
    group_ = CancelToken::create();
}

ProxyStatus ProxyManager::status(MediaId media) const {
    std::lock_guard lock(mutex_);
    auto it = status_.find(media);
    return it == status_.end() ? ProxyStatus{} : it->second;
}

}  // namespace avc::proxy
