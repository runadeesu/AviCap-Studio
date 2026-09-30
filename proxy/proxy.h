#pragma once
// Proxy media: low-resolution, intra-frame (MJPEG) copies of the video stream
// for smooth editing of heavy footage. Proxies keep the original timestamps so
// they swap in frame-accurately; audio always plays from the original file and
// export always uses the original media.

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/jobs.h"
#include "core/result.h"
#include "timeline/model.h"

namespace avc::proxy {

struct ProxyRequest {
    std::string source;
    std::string output;
    int height = 540;  // 360 / 540 / 720 / 1080
    int quality = 4;   // MJPEG qscale (2 best .. 31 worst)
};

// Transcodes the primary video stream. `progress` receives 0..1.
Status generateProxy(const ProxyRequest& req, const CancelToken& cancel, const std::function<void(float)>& progress = {});

int presetHeight(const std::string& preset);  // "540p" -> 540

enum class ProxyState { None, Queued, Running, Ready, Failed };

struct ProxyStatus {
    ProxyState state = ProxyState::None;
    float progress = 0;
    std::string path;
    std::string error;
};

// Owns proxy files in the cache directory (keyed by source identity and size)
// and schedules generation, preferably in a worker process (avicap-cli) so a
// decoder crash cannot take the editor down.
class ProxyManager {
public:
    explicit ProxyManager(std::filesystem::path directory);
    ~ProxyManager();

    // Existing, up-to-date proxy for the media item or "".
    std::string proxyPath(const MediaItem& media, int height) const;
    void request(const MediaItem& media, int height);
    void cancelAll();
    [[nodiscard]] ProxyStatus status(MediaId media) const;
    void setWorkerExecutable(std::string exe) { workerExe_ = std::move(exe); }
    [[nodiscard]] std::filesystem::path directory() const { return dir_; }

private:
    std::filesystem::path fileFor(const MediaItem& media, int height) const;
    std::filesystem::path dir_;
    std::string workerExe_;
    mutable std::mutex mutex_;
    std::map<MediaId, ProxyStatus> status_;
    std::map<MediaId, JobHandle> jobs_;
    CancelToken group_ = CancelToken::create();
};

}  // namespace avc::proxy
