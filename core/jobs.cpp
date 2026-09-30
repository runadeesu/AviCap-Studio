#include "core/jobs.h"

#include <algorithm>

#include "core/log.h"
#include "core/platform.h"

namespace avc {

// ---------------------------------------------------------------- CancelToken

CancelToken CancelToken::create() { return CancelToken(std::make_shared<State>()); }

bool CancelToken::cancelled() const noexcept {
    for (const State* s = state_.get(); s; s = s->parent.get())
        if (s->flag.load(std::memory_order_acquire)) return true;
    return false;
}

CancelToken CancelToken::child() const {
    auto s = std::make_shared<State>();
    s->parent = state_;
    return CancelToken(std::move(s));
}

void CancelToken::cancel() const noexcept {
    if (state_) state_->flag.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------- JobState

void JobState::setMessage(std::string m) {
    std::lock_guard lock(mutex_);
    message_ = std::move(m);
}

std::string JobState::message() const {
    std::lock_guard lock(mutex_);
    return message_;
}

void JobState::setError(std::string e) {
    std::lock_guard lock(mutex_);
    error_ = std::move(e);
}

std::string JobState::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void JobState::markFinished(JobStatus s) {
    {
        std::lock_guard lock(mutex_);
        status.store(s);
    }
    cv_.notify_all();
}

bool JobState::waitFor(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return finished(); });
}

void JobState::wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return finished(); });
}

// ---------------------------------------------------------------- ThreadPool

namespace {
// Max-heap comparator: "a < b" means a runs after b.
bool itemLess(int pa, uint64_t sa, int pb, uint64_t sb) {
    if (pa != pb) return pa > pb;  // lower enum value = higher priority
    return sa > sb;                // FIFO within a priority
}
}  // namespace

ThreadPool::ThreadPool(std::string name, unsigned threadCount) : name_(std::move(name)) {
    threadCount = std::max(1u, threadCount);
    threads_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) threads_.emplace_back([this, i] { workerLoop(i); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& it : heap_) {
            it.state->token.cancel();
            it.state->markFinished(JobStatus::Cancelled);
        }
        heap_.clear();
        for (auto& r : running_) r->token.cancel();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

JobHandle ThreadPool::submit(std::string name, JobPriority prio, JobFn fn, const CancelToken& parent) {
    auto state = std::make_shared<JobState>();
    state->name = std::move(name);
    state->priority = prio;
    state->token = parent.valid() ? parent.child() : CancelToken::create();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            state->markFinished(JobStatus::Cancelled);
            return JobHandle(state);
        }
        heap_.push_back(Item{state, std::move(fn), seq_++});
        std::push_heap(heap_.begin(), heap_.end(), [](const Item& a, const Item& b) {
            return itemLess(static_cast<int>(a.state->priority), a.seq, static_cast<int>(b.state->priority), b.seq);
        });
    }
    cv_.notify_one();
    return JobHandle(state);
}

void ThreadPool::cancelAll() {
    std::lock_guard lock(mutex_);
    for (auto& it : heap_) {
        it.state->token.cancel();
        it.state->markFinished(JobStatus::Cancelled);
    }
    heap_.clear();
    for (auto& r : running_) r->token.cancel();
    idleCv_.notify_all();
}

size_t ThreadPool::pendingCount() const {
    std::lock_guard lock(mutex_);
    return heap_.size();
}

size_t ThreadPool::runningCount() const {
    std::lock_guard lock(mutex_);
    return running_.size();
}

std::vector<std::shared_ptr<JobState>> ThreadPool::activeJobs() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<JobState>> out = running_;
    for (auto& it : heap_) out.push_back(it.state);
    return out;
}

void ThreadPool::waitIdle() {
    std::unique_lock lock(mutex_);
    idleCv_.wait(lock, [this] { return heap_.empty() && running_.empty(); });
}

void ThreadPool::workerLoop(unsigned index) {
    setCurrentThreadName((name_ + "-" + std::to_string(index)).c_str());
    for (;;) {
        Item item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !heap_.empty(); });
            if (stopping_ && heap_.empty()) return;
            std::pop_heap(heap_.begin(), heap_.end(), [](const Item& a, const Item& b) {
                return itemLess(static_cast<int>(a.state->priority), a.seq, static_cast<int>(b.state->priority),
                                b.seq);
            });
            item = std::move(heap_.back());
            heap_.pop_back();
            running_.push_back(item.state);
        }
        JobState& st = *item.state;
        if (st.token.cancelled()) {
            st.markFinished(JobStatus::Cancelled);
        } else {
            st.status.store(JobStatus::Running);
            JobContext ctx(st);
            JobStatus result = JobStatus::Succeeded;
            try {
                item.fn(ctx);
                if (st.token.cancelled()) result = JobStatus::Cancelled;
                else if (!st.error().empty()) result = JobStatus::Failed;
            } catch (const std::exception& e) {
                st.setError(e.what());
                result = JobStatus::Failed;
                AVC_ERROR("jobs", "Job '{}' failed: {}", st.name, e.what());
            } catch (...) {
                st.setError("unknown exception");
                result = JobStatus::Failed;
                AVC_ERROR("jobs", "Job '{}' failed with unknown exception", st.name);
            }
            if (result == JobStatus::Succeeded) st.progress.store(1.0f);
            st.markFinished(result);
        }
        item.fn = nullptr;  // release captures outside the lock
        {
            std::lock_guard lock(mutex_);
            running_.erase(std::remove(running_.begin(), running_.end(), item.state), running_.end());
            if (heap_.empty() && running_.empty()) idleCv_.notify_all();
        }
    }
}

// ---------------------------------------------------------------- Jobs

namespace {
struct Pools {
    std::mutex mutex;
    std::unique_ptr<ThreadPool> decode, cache, analysis, io;
};
Pools& pools() {
    static Pools p;
    return p;
}
ThreadPool& getPool(std::unique_ptr<ThreadPool>& slot, const char* name, unsigned threads) {
    std::lock_guard lock(pools().mutex);
    if (!slot) slot = std::make_unique<ThreadPool>(name, threads);
    return *slot;
}
}  // namespace

ThreadPool& Jobs::decode() { return getPool(pools().decode, "decode", std::max(2u, hardwareThreads() / 2)); }
ThreadPool& Jobs::cache() { return getPool(pools().cache, "cache", std::clamp(hardwareThreads() / 4, 1u, 3u)); }
ThreadPool& Jobs::analysis() { return getPool(pools().analysis, "analysis", std::clamp(hardwareThreads() / 4, 1u, 2u)); }
ThreadPool& Jobs::io() { return getPool(pools().io, "io", 2); }

void Jobs::shutdown() {
    std::unique_ptr<ThreadPool> d, c, a, i;
    {
        std::lock_guard lock(pools().mutex);
        d = std::move(pools().decode);
        c = std::move(pools().cache);
        a = std::move(pools().analysis);
        i = std::move(pools().io);
    }
    // Destructors cancel and join.
}

std::vector<std::shared_ptr<JobState>> Jobs::allActiveJobs() {
    std::vector<std::shared_ptr<JobState>> out;
    std::lock_guard lock(pools().mutex);
    for (auto* p : {pools().decode.get(), pools().cache.get(), pools().analysis.get(), pools().io.get()}) {
        if (!p) continue;
        auto j = p->activeJobs();
        out.insert(out.end(), j.begin(), j.end());
    }
    return out;
}

}  // namespace avc
