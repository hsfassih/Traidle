#pragma once

#include "candlesticks.h"
#include "indicators.h"
#include "smc-ict/smc_engine.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// visualization: a self-contained WebSocket broadcast server that streams
// closed-candle + indicator (+ eventually prediction) data to a single local
// browser tab for real-time charting.
//
// This module is deliberately a one-way, fire-and-forget data sink from the
// point of view of everything else in the project: binance.cpp and
// historical_get.cpp only ever call VisualizationServer::push()/
// seedHistory(), both of which are thread-safe, non-blocking, and never
// throw - a stalled browser, a closed tab, or even a failed server bind can
// never delay or break Binance ingestion, CSV writes, or indicator
// computation. See visualization.cpp for the full threading design.
//
// Single-viewer design: this project is strictly single-viewer/local, so
// the server tracks at most one active browser session at a time. A new
// connection simply replaces whatever was active before it (see
// visualization.cpp) - there is no session registry, per-session strand,
// or fan-out logic, since none of that machinery earns its cost with only
// ever one viewer. If that assumption ever changes, this is the module to
// revisit first.
//
// --- SMC/ICT additions ---
// smc-ict/'s detected structures (swings, FVGs, Order Blocks, BOS/CHoCH,
// liquidity, PDH/PDL, Premium/Discount+OTE) are deliberately NOT folded
// into VisualizationMessage / the existing 50,000-candle history ring
// buffer (candleFieldsToJson, historyByTimeframe). Two reasons: (1) per
// smc-ict/'s design, none of this is meant to be persisted or replicated
// once per candle row - it is computed live, purely for display; bolting
// it onto every one of 50,000 buffered candle rows would mean re-sending
// the same still-active zone thousands of times instead of once. (2) an
// active zone can remain relevant for far longer than one candle, so it
// needs its own "current state, replaced wholesale" store
// (smcByTimeframe_) rather than a per-candle ring buffer entry. See
// updateSmcSnapshot()/pushSmcEvents() below and their .cpp implementation.
// ---------------------------------------------------------------------------
namespace visualization {

// Reserved for the (not yet implemented) ICT/model prediction layer. Kept
// here now, fully optional on every VisualizationMessage, so wiring in the
// real prediction engine later is purely additive - no schema break, no
// frontend change beyond reading fields that already exist.
struct PredictionZone {
    string label;                  // e.g. "bullish_order_block", "fvg_bearish"
    double priceHigh = 0.0;
    double priceLow = 0.0;
    int64_t startTimeMs = 0;
    optional<int64_t> endTimeMs;   // nullopt = open-ended / still active
};

struct PredictionSnapshot {
    optional<double> entryLong;
    optional<double> entryShort;
    optional<double> takeProfitLong;
    optional<double> takeProfitShort;
    optional<double> stopLossLong;
    optional<double> stopLossShort;
    vector<PredictionZone> zones;
};

// One broadcastable unit of chart data: everything the frontend needs to
// render one candle's worth of candlestick, volume, and indicator
// information, plus an optional prediction overlay. Deliberately a plain,
// flat struct (not reusing candlesticks::Candlestick / indicators::
// IndicatorSnapshot by inheritance) so this module's wire format can
// evolve independently of the CSV schema.
struct VisualizationMessage {
    string symbol;
    string timeframe;   // raw label, e.g. "1m", exactly as typed at startup
    int64_t openTime = 0;
    int64_t closeTime = 0;
    bool closed = true;  // false while this candle is still forming; the
                          // frontend uses this only for display (e.g. a
                          // "LIVE" badge) - indicatorValues being all-null
                          // is what actually gates indicator-line updates
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double baseVolume = 0.0;
    double quoteVolume = 0.0;
    double takerBuyBaseVolume = 0.0;
    double takerBuyQuoteVolume = 0.0;
    indicators::IndicatorSnapshot indicatorValues;
    optional<PredictionSnapshot> prediction;  // nullopt until the prediction engine exists
};

// Serializes one message to its wire-format JSON string. Exposed publicly
// so the (future) prediction engine, tests, or anything else can inspect
// the exact wire format without spinning up a server.
string toJson(const VisualizationMessage& message);

// Serializes an SmcSnapshot / SmcUpdate to their wire-format JSON strings -
// exposed publicly for the same reason as toJson() above (inspectable
// without a server, usable by tests).
string smcSnapshotToJson(const string& symbol, const string& timeframe,
                        const smc::SmcSnapshot& snapshot);
string smcUpdateToJson(const string& symbol, const string& timeframe, const smc::SmcUpdate& update);

class VisualizationServer {
public:
    explicit VisualizationServer(unsigned short port);
    ~VisualizationServer();

    VisualizationServer(const VisualizationServer&) = delete;
    VisualizationServer& operator=(const VisualizationServer&) = delete;

    // Binds the port and starts the dedicated server thread. If the bind
    // fails (e.g. port already in use), this logs a warning and returns
    // without throwing - push()/seedHistory() remain safe no-ops in that
    // case, so the rest of the application is entirely unaffected. Safe to
    // call once; a second call is a no-op.
    void start();

    // Stops the server thread and joins it. Safe to call from the
    // destructor or explicitly at shutdown; safe to call even if start()
    // was never called or failed to bind.
    void stop();

    // Thread-safe, non-blocking, never throws. Called by a timeframe
    // worker with EVERY candle it sees - both still-forming ticks and the
    // final closed candle: enqueues it for broadcast to the active session
    // (if any) AND records/overwrites it in that timeframe's in-memory
    // history ring buffer, keyed by open time (see seedHistory() for the
    // backfill-time equivalent that skips the broadcast step).
    void push(VisualizationMessage message);

    // Thread-safe, non-blocking, never throws. Called during startup
    // replay/backfill (historical_get.cpp) for every candle it processes,
    // purely to populate the history ring buffer so a browser connecting
    // right after startup immediately has recent context to draw. Never
    // broadcast live - these are not "just happened" events.
    void seedHistory(VisualizationMessage message);

    // Thread-safe, non-blocking, never throws. Replaces (wholesale, not
    // merged) the CURRENT set of active SMC/ICT structures for one
    // timeframe. Called once after backfill/replay completes, and again
    // any time smc::SmcEngine::currentSnapshot() meaningfully changes
    // (binance.cpp calls this after every live update() too - it is cheap:
    // see smc_engine.h's active-list size discussion). NOT broadcast
    // directly; sent to a newly-connected session alongside its "history"
    // message (see sendHistorySnapshot() in the .cpp), and replaces
    // whatever this timeframe's snapshot previously held so a browser that
    // connects later never sees stale structures re-sent.
    void setSmcSnapshot(const string& symbol, const string& timeframe, smc::SmcSnapshot snapshot);

    // Thread-safe, non-blocking, never throws. Broadcasts a live SMC/ICT
    // delta (new/changed swings, FVGs, Order Blocks, structural events,
    // equal levels, PDH/PDL, Premium/Discount+OTE) to the active session,
    // the same way push() broadcasts a live candle - but on its own queue,
    // since smc::SmcUpdate is a different shape from VisualizationMessage
    // and most candles produce a non-trivial one here (see smc_engine.h's
    // SmcUpdate::empty() - this project's real data shows the vast
    // majority of closed candles produce SOME update, mostly Rejection
    // Blocks and FVG mitigation-status changes, so this is not a rare-event
    // queue; the frontend's SMC toggles are what keep the resulting
    // traffic from being overwhelming to look at, not the wire protocol).
    // A caller should skip this call entirely when update.empty() is true.
    void pushSmcEvent(const string& symbol, const string& timeframe, smc::SmcUpdate update);

private:
    struct Impl;
    unique_ptr<Impl> impl_;
};

}  // namespace visualization
