#pragma once
// Priority background job system with cooperative cancellation.
//
// Heavy work (decode, thumbnails, waveforms, proxies, analysis, AI, export)
// must never run on the UI thread. Each pool orders pending jobs by priority,
// then FIFO. Jobs observe cancellation through JobContext; closing a project
// cancels its JobGroup so no stale work survives.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace avc {

enum class JobPriority : int {
    Highest = 0,     // playback decode
    High = 1,        // visible thumbnails, interactive requests
    Medium = 2,      // waveforms
    Low = 3,         // proxies, optimized media
    Background = 4,  // analysis, AI, cache maintenance
};

// Shared cancellation flag; tokens may chain to a parent (e.g. a project group).
class CancelToken {
public:
    CancelToken() = default;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    [[nodiscard]] CancelToken child() const;  // cancelled when this one is
    void cancel() const noexcept;
    static CancelToken create();

private:
    struct State {
        std::atomic<bool> flag{false};
        std::shared_ptr<State> parent;
    };
    explicit CancelToken(std::shared_ptr<State> s) : state_(std::move(s)) {}
    std::shared_ptr<State> state_;
};

enum class JobStatus { Pending, Running, Succeeded, Failed, Cancelled };

class JobState {
public:
    std::string name;
    JobPriority priority = JobPriority::Medium;
    CancelToken token;
    std::atomic<JobStatus> status{JobStatus::Pending};
    std::atomic<float> progress{0.0f};

    void setMessage(std::string m);
    std::string message() const;
    void setError(std::string e);
    std::string error() const;
    bool finished() const noexcept {
        const JobStatus s = status.load();
        return s == JobStatus::Succeeded || s == JobStatus::Failed || s == JobStatus::Cancelled;
    }
    void markFinished(JobStatus s);
    bool waitFor(std::chrono::milliseconds timeout);
    void wait();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string message_;
    std::string error_;
};

class JobContext {
public:
    explicit JobContext(JobState& s) : state_(s) {}
    [[nodiscard]] bool cancelled() const noexcept { return state_.token.cancelled(); }
    void setProgress(float f) noexcept { state_.progress.store(f); }
    void setMessage(std::string m) { state_.setMessage(std::move(m)); }
    const CancelToken& token() const noexcept { return state_.token; }
    JobState& state() noexcept { return state_; }

private:
    JobState& state_;
};

class JobHandle {
public:
    JobHandle() = default;
    explicit JobHandle(std::shared_ptr<JobState> s) : state_(std::move(s)) {}
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    [[nodiscard]] JobStatus status() const noexcept { return state_ ? state_->status.load() : JobStatus::Cancelled; }
    [[nodiscard]] bool finished() const noexcept { return !state_ || state_->finished(); }
    [[nodiscard]] float progress() const noexcept { return state_ ? state_->progress.load() : 0.0f; }
    void cancel() const noexcept {
        if (state_) state_->token.cancel();
    }
    void wait() const {
        if (state_) state_->wait();
    }
    bool waitFor(std::chrono::milliseconds t) const { return !state_ || state_->waitFor(t); }
    const std::shared_ptr<JobState>& state() const noexcept { return state_; }

private:
    std::shared_ptr<JobState> state_;
};

using JobFn = std::function<void(JobContext&)>;

class ThreadPool {
public:
    ThreadPool(std::string name, unsigned threadCount);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // `parent` (optional) links the job's cancellation to a group token.
    JobHandle submit(std::string name, JobPriority prio, JobFn fn, const CancelToken& parent = {});
    void cancelAll();
    size_t pendingCount() const;
    size_t runningCount() const;
    unsigned threadCount() const noexcept { return static_cast<unsigned>(threads_.size()); }
    const std::string& name() const noexcept { return name_; }
    // Snapshot of queued + running jobs (for the background-tasks UI).
    std::vector<std::shared_ptr<JobState>> activeJobs() const;
    // Blocks until the queue is empty and no job is running.
    void waitIdle();

private:
    struct Item {
        std::shared_ptr<JobState> state;
        JobFn fn;
        uint64_t seq = 0;
    };
    void workerLoop(unsigned index);

    std::string name_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::vector<Item> heap_;  // priority heap
    std::vector<std::shared_ptr<JobState>> running_;
    std::vector<std::thread> threads_;
    uint64_t seq_ = 0;
    bool stopping_ = false;
};

// Process-wide pools. Created lazily; shut down explicitly at exit.
class Jobs {
public:
    static ThreadPool& decode();    // playback / scrubbing decode
    static ThreadPool& cache();     // thumbnails, waveforms, proxies
    static ThreadPool& analysis();  // AI / audio / scene analysis
    static ThreadPool& io();        // file IO, network fetches
    static void shutdown();
    static std::vector<std::shared_ptr<JobState>> allActiveJobs();
};

}  // namespace avc
