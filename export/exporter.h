#pragma once
// Export job and export queue. Exports never run on the UI thread: the queue
// owns a dedicated worker thread and each job uses its own GPU device,
// decoders and mixer so preview keeps working during an export.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/jobs.h"
#include "export/export_settings.h"
#include "export/verify.h"
#include "render/gpu/gpu.h"
#include "timeline/model.h"

namespace avc::exp {

enum class ExportState { Queued, Preparing, Rendering, Finalizing, Verifying, Done, Failed, Cancelled, Paused };
const char* exportStateName(ExportState s);

struct ExportProgress {
    ExportState state = ExportState::Queued;
    int64_t framesDone = 0;
    int64_t totalFrames = 0;
    double elapsedSec = 0;
    double etaSec = 0;
    double fps = 0;
    double speed = 0;  // x realtime
    uint64_t bytes = 0;
    std::string encoder;
    std::string message;
    [[nodiscard]] float fraction() const {
        return totalFrames > 0 ? static_cast<float>(framesDone) / static_cast<float>(totalFrames) : 0.0f;
    }
};

using DeviceFactory = std::function<std::unique_ptr<gpu::Device>()>;
// Default: Direct3D 11 (new device) on Windows, CPU elsewhere.
DeviceFactory defaultDeviceFactory();

class ExportJob {
public:
    ExportJob(ProjectPtr project, SequenceId sequence, ExportSettings settings, DeviceFactory devices = defaultDeviceFactory());

    // Runs the export on the calling thread (the queue worker).
    Status run(const CancelToken& cancel);
    void pause();
    void resume();
    [[nodiscard]] bool paused() const { return paused_.load(); }

    [[nodiscard]] ExportProgress progress() const;
    [[nodiscard]] const ExportSettings& settings() const { return settings_; }
    [[nodiscard]] const VerifyReport& report() const { return report_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] Id id() const { return id_; }
    // Clears progress so the job can be run again (retry).
    void resetForRetry();
    // Marks a queued job as cancelled without running it.
    void markCancelled() { setState(ExportState::Cancelled, "Cancelled"); }

private:
    void setState(ExportState s, std::string msg = {});
    Id id_;
    std::string name_;
    ProjectPtr project_;
    SequenceId sequence_;
    ExportSettings settings_;
    DeviceFactory devices_;
    mutable std::mutex mutex_;
    ExportProgress progress_;
    VerifyReport report_;
    std::atomic<bool> paused_{false};
    std::condition_variable pauseCv_;
    std::mutex pauseMutex_;
};

class ExportQueue {
public:
    ExportQueue();
    ~ExportQueue();

    Id add(std::shared_ptr<ExportJob> job);
    bool remove(Id id);  // queued / finished jobs only
    bool moveUp(Id id);
    bool moveDown(Id id);
    void cancel(Id id);
    void pause(Id id);
    void resume(Id id);
    bool retry(Id id);
    [[nodiscard]] std::vector<std::shared_ptr<ExportJob>> jobs() const;
    [[nodiscard]] bool busy() const;
    void cancelAll();
    // Called on the worker thread when a job finishes.
    void setOnFinished(std::function<void(const ExportJob&)> fn);

private:
    void worker();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<ExportJob>> jobs_;
    std::shared_ptr<ExportJob> current_;
    CancelToken currentCancel_;
    std::function<void(const ExportJob&)> onFinished_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace avc::exp
