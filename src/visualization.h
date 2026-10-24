#pragma once

#include "utils/candlesticks.h"
#include "utils/indicators.h"
#include "smc-ict/smc_engine.h"

#include <cstdint>
#include <functional>
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
// history backfill and live-stream modules only ever call VisualizationServer::push()/
// seedHistory(), both of which are thread-safe, non-blocking, and never
// throw - a stalled browser, a closed tab, or even a failed server bind can
// never delay or break Binance ingestion, CSV writes, or indicator
// computation. See visualization.cpp for the full threading design.
//
// --- Control channel (market picker / Disconnect) ---
// The only thing that flows the other way is a small control channel: a
// browser may send text frames (see setCommandHandler()), and the application
// tells every viewer which phase the session is in (setSessionState()). The
// server itself knows nothing about markets, symbols or what a command means -
// it just hands the text to whoever registered the handler (session_manager.h)
// and relays the state string it is given. Between two sessions the buffered
// chart data is wiped (resetSessionData()), so a new symbol never inherits the
// previous one's candles.
//
// Multi-viewer design: any number of browser tabs/windows can connect
// concurrently, each getting its own session, its own initial history/SMC
// snapshot, and every subsequent live broadcast independently (see
// visualization.cpp's `sessions` collection and drainQueue()/
// drainSmcQueue()). There is still no per-session strand or locking
// needed: every Session is only ever touched from the server's one
// dedicated io_context thread, so fan-out is just a loop over the current
// session list on that same thread.
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

// Whether a WebSocket page with this `Origin` header may send control commands.
// Anything can still CONNECT and watch, exactly as before; only commands are
// restricted, because they can start a session. Allowed: no Origin at all (a
// non-browser client), the literal "null" (a page opened from disk, file://),
// and localhost / 127.0.0.1 / [::1] on any scheme and port. A web page on some
// other site - which a browser lets open ws://localhost:8765 - is refused.
bool originAllowsCommands(const string& origin);

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
    // final closed candle: enqueues it for broadcast to every currently
    // connected session (if any) AND records/overwrites it in that
    // timeframe's in-memory history ring buffer, keyed by open time (see
    // seedHistory() for the backfill-time equivalent that skips the
    // broadcast step).
    void push(VisualizationMessage message);

    // Thread-safe, non-blocking, never throws. Called during startup
    // replay/backfill modules for every candle they process,
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

    // Called with the text of every control message a viewer sends (a JSON
    // document; this class neither parses nor validates it). Runs on the
    // server's own I/O thread, so it MUST NOT block - hand the text to another
    // thread and return. Set it before start(); pass an empty function to
    // detach (when it returns, no call is in flight and none will start).
    // Messages from a page whose Origin is not allowed (see
    // originAllowsCommands()) are dropped before they get here.
    using CommandHandler = function<void(const string& messageText)>;
    void setCommandHandler(CommandHandler handler);

    // Thread-safe, never throws. Stores `stateJson` (an opaque, ready-to-send
    // JSON document - the application builds it, see session_manager.cpp),
    // broadcasts it to every connected viewer, and makes it the FIRST message
    // any viewer connecting later receives (before its history snapshot).
    void setSessionState(const string& stateJson);

    // Thread-safe, never throws. Sends one timeframe's current history and SMC
    // snapshot to every viewer that is ALREADY connected - the same two
    // messages a newly connecting viewer gets for it. Workers call this once
    // their startup backfill/replay is done: with the browser now connecting
    // BEFORE a session starts (instead of after), history seeded during backfill
    // would otherwise never reach it. No-op without viewers.
    void publishTimeframe(const string& timeframe);

    // Thread-safe, never throws. Forgets every buffered candle, SMC snapshot
    // and queued-but-unsent message. Called between two sessions, after all of
    // the previous session's workers have stopped, so the next symbol starts
    // from nothing. Viewers stay connected.
    void resetSessionData();

    // True once start() has bound the port and the server thread is serving.
    bool isRunning() const;

    // Thread-safe, non-blocking, never throws. Broadcasts a live SMC/ICT
    // delta (new/changed swings, FVGs, Order Blocks, structural events,
    // equal levels, PDH/PDL, Premium/Discount+OTE) to every currently
    // connected session, the same way push() broadcasts a live candle - but on its own queue,
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