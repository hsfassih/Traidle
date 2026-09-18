#include "smc_thread_pool.h"

#include <algorithm>

namespace smc {

ThreadPool& ThreadPool::instance() {
    static ThreadPool pool;
    return pool;
}

ThreadPool::ThreadPool() {
    // Leave at least one core free for the io_context threads, WS session
    // I/O, and everything else already running - this pool only needs to
    // win on the CPU-bound bulk pre-scan, not monopolize the machine.
    const unsigned hw = thread::hardware_concurrency();
    const size_t count = hw > 1 ? static_cast<size_t>(hw - 1) : 1;
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        lock_guard<mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPool::workerLoop() {
    for (;;) {
        function<void()> task;
        {
            unique_lock<mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

}  // namespace smc
