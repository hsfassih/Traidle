#pragma once

#include "candlesticks.h"
#include "indicators.h"

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
// render one closed candle's worth of candlestick, volume, and indicator
// information, plus an optional prediction overlay. Deliberately a plain,
// flat struct (not reusing candlesticks::Candlestick / indicators::
// IndicatorSnapshot by inheritance) so this module's wire format can
// evolve independently of the CSV schema.
struct VisualizationMessage {
    string symbol;
    string timeframe;   // raw label, e.g. "1m", exactly as typed at startup
    int64_t openTime = 0;
    int64_t closeTime = 0;
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
    // worker with a freshly closed live candle: enqueues it for broadcast
    // to the active session (if any) AND records it into that
    // timeframe's in-memory history ring buffer (see seedHistory() for
    // the backfill-time equivalent that skips the broadcast step).
    void push(VisualizationMessage message);

    // Thread-safe, non-blocking, never throws. Called during startup
    // replay/backfill (historical_get.cpp) for every candle it processes,
    // purely to populate the history ring buffer so a browser connecting
    // right after startup immediately has recent context to draw. Never
    // broadcast live - these are not "just happened" events.
    void seedHistory(VisualizationMessage message);

private:
    struct Impl;
    unique_ptr<Impl> impl_;
};

}  // namespace visualization