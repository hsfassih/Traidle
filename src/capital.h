#pragma once

#include "candle_builder.h"
#include "capital_api.h"
#include "pipeline_common.h"
#include "visualization.h"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// capital: the Capital.com (Forex/CFD) market adapter - the counterpart of
// binance.cpp. One shared quote socket (the feed thread) fans every quote out
// to one worker thread per timeframe; each worker is a fully isolated copy of
// the Binance per-timeframe pipeline (own io_context, IndicatorEngine,
// SmcEngine, CSV, backfill), so 15 timeframes of indicator/SMC/CSV/visualizer
// work run in parallel and never serialize behind one thread.
//
//   Capital WebSocket --> [feed thread: parse 1 quote, post to N workers]
//                              |--> worker 1m  --> candles/CSV/indicators/SMC/visualizer
//                              |--> worker 5m  --> ...
//                              '--> worker 1M  --> ...
// ---------------------------------------------------------------------------
namespace capital {

// One bid/ask quote, exactly as it left the WebSocket.
struct Tick {
    int64_t tsMs = 0;          // the quote's OWN timestamp (Capital's clock), epoch ms
    double bid = 0;            // candles are built from the bid side everywhere
    double ask = 0;
    int64_t recvSteadyNs = 0;  // steady_clock at receipt on the feed thread (hand-off latency)
};

// The feed's handle on one worker. The feed posts onto `ioc` (the worker's
// own io_context); a worker sets `closed` under `m` BEFORE destroying that
// io_context, so the feed can never post to a dead context.
struct TickSink {
    mutex m;
    bool closed = false;
    net::io_context* ioc = nullptr;
    function<void(const Tick&)> onTick;
    // Delivered after the socket was (re)established and the subscription
    // acknowledged; the argument is the server-clock time of that moment. A
    // worker whose forming-candle seed is older than that may have missed ticks
    // and re-seeds from REST. (The first connection is announced too: a worker
    // that seeded after it simply ignores the event.)
    function<void(int64_t /*connectedAtServerMs*/)> onReconnected;
};

// Loads credentials, logs in, measures the server clock offset and checks the
// epic exists. Prints informational lines (market status) to stdout and
// returns the logged-in Session; returns nullptr and fills `errorText` (a clear
// one-liner, never containing secrets) on failure. Call it BEFORE creating the
// TerminalBoard so errors print as plain text.
shared_ptr<Session> preflight(const string& epic, string& errorText);
// Same, with explicit credentials/endpoints (used by the offline mock-server test).
shared_ptr<Session> preflightWith(Credentials credentials, Endpoints endpoints, const string& epic,
                                  string& errorText);

struct RunOptions {
    // Normally empty: run for the life of the process (Ctrl+C). Tests and the
    // probe set it so run() stops the feed and workers and returns.
    optional<chrono::milliseconds> runFor;
};

// Starts the feed thread and one worker per task, then blocks (joins them).
void run(shared_ptr<Session> session, const string& epic,
         const vector<pipeline::TimeframeTask>& tasks, const filesystem::path& folder,
         pipeline::TerminalBoard& board, visualization::VisualizationServer& vizServer,
         const RunOptions& options = RunOptions{});

}  // namespace capital