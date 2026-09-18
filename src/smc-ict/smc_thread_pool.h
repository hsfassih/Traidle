#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// smc_thread_pool: ONE process-wide, bounded thread pool, shared by every
// timeframe's SmcEngine, used for exactly one thing - the parallel chunked
// bulk-history Swing/FVG FORMATION pre-scan that runs once per timeframe at
// startup/backfill time (see smc_engine.cpp's bulkPrescan()). Deliberately
// NOT used anywhere on the live, per-candle-close path - see the
// parallelism design note in smc_engine.h for the reasoning (that path's
// own per-candle cost is measured at ~1-2us for the entire six-module
// pipeline; dispatching that to a thread pool would cost more in
// scheduling overhead than it could ever save).
//
// Sized once to std::thread::hardware_concurrency() and shared (mirroring
// historical_get.cpp's process-wide RateLimiter::instance() singleton
// pattern) rather than each timeframe spinning up its own pool - N
// timeframes each independently grabbing hardware_concurrency() threads
// would oversubscribe the machine badly the moment more than one timeframe
// backfills at once, which is the common case (every timeframe backfills
// concurrently on startup).
// ---------------------------------------------------------------------------
namespace smc {

class ThreadPool {
public:
    static ThreadPool& instance();

    // Submits `task` and returns a future for its result. Safe to call
    // from any thread, including from within another task running on this
    // same pool (a task can submit sub-tasks) - though see bulkPrescan()
    // for why this module never actually needs to.
    template <typename F, typename R = std::invoke_result_t<F>>
    future<R> submit(F task) {
        auto packaged = make_shared<packaged_task<R()>>(move(task));
        future<R> result = packaged->get_future();
        {
            lock_guard<mutex> lock(mutex_);
            tasks_.push([packaged]() { (*packaged)(); });
        }
        cv_.notify_one();
        return result;
    }

    size_t threadCount() const { return workers_.size(); }

    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    ThreadPool();
    void workerLoop();

    vector<thread> workers_;
    queue<function<void()>> tasks_;
    mutex mutex_;
    condition_variable cv_;
    bool stopping_ = false;
};

}  // namespace smc
