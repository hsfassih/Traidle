#pragma once

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// app::StopSignal: the one "please stop" switch for a market session (one
// symbol, N timeframe workers, optionally a shared feed).
//
// It generalises what capital.cpp's old private StopControl did, and is what
// lets the browser's Disconnect button tear a session down without ending the
// process:
//   * every worker registers the io_context it runs (add/remove); request()
//     stops all of them, which makes their ioc.run() return;
//   * a plain atomic flag (flag()) is handed to the blocking startup work
//     (CSV replay, history backfill) so that it can notice the request between
//     steps - an io_context cannot interrupt those, they never run on it;
//   * waitFor()/wait() let a controlling thread sleep until the request.
//
// Everything is thread-safe. request() is idempotent.
// ---------------------------------------------------------------------------
namespace app {

class StopSignal {
public:
    // Asks every registered worker to stop. Safe from any thread, any number of times.
    void request() {
        std::lock_guard<std::mutex> lock(m_);
        stopped_.store(true);
        for (auto* ioc : contexts_) {
            ioc->stop();
        }
        cv_.notify_all();
    }

    bool stopped() const { return stopped_.load(); }

    // For long blocking loops that cannot be interrupted through an io_context
    // (see historical::ensureContinuousHistory's `cancel` parameter).
    const std::atomic<bool>* flag() const { return &stopped_; }

    // A worker registers its io_context right after creating it and removes it
    // before the context is destroyed. Registering after request() stops the
    // context immediately, so ioc.run() returns at once.
    void add(boost::asio::io_context* ioc) {
        std::lock_guard<std::mutex> lock(m_);
        if (stopped_.load()) {
            ioc->stop();
        } else {
            contexts_.push_back(ioc);
        }
    }

    void remove(boost::asio::io_context* ioc) {
        std::lock_guard<std::mutex> lock(m_);
        contexts_.erase(std::remove(contexts_.begin(), contexts_.end(), ioc), contexts_.end());
    }

    // Blocks until request() is called or `timeout` passes; returns whether
    // the stop was requested.
    bool waitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, timeout, [this] { return stopped_.load(); });
    }

    // Blocks until request() is called.
    void wait() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return stopped_.load(); });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> stopped_{false};
    std::vector<boost::asio::io_context*> contexts_;
};

// Runs a callable when it goes out of scope (C++17 has no std::scope_exit).
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() { f_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F f_;
};
template <class F>
ScopeExit<F> onScopeExit(F f) {
    return ScopeExit<F>(std::move(f));
}

}  // namespace app