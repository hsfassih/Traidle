#include "visualization.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

namespace visualization {
namespace {

constexpr size_t kMaxQueueSize = 500;      // bounded outbound broadcast queue (drop-oldest when full)
constexpr size_t kMaxSmcQueueSize = 500;   // same drop-oldest policy, separate queue - see class comment in visualization.h
constexpr size_t kHistoryDepth = 50000;    // distinct candles retained per timeframe for snapshot-on-connect
constexpr auto kHeartbeatInterval = seconds(15);  // sent only if nothing real went out in this window

json optionalToJson(const optional<double>& value) {
    return value.has_value() ? json(*value) : json(nullptr);
}

json optionalMsToJson(const optional<int64_t>& value) {
    return value.has_value() ? json(*value) : json(nullptr);
}

json indicatorSnapshotToJson(const indicators::IndicatorSnapshot& snapshot) {
    json j;
    j["sma"] = optionalToJson(snapshot.sma);
    j["ema"] = optionalToJson(snapshot.ema);
    j["macd_line"] = optionalToJson(snapshot.macdLine);
    j["macd_signal"] = optionalToJson(snapshot.macdSignal);
    j["macd_histogram"] = optionalToJson(snapshot.macdHistogram);
    j["rsi"] = optionalToJson(snapshot.rsi);
    j["stoch_k"] = optionalToJson(snapshot.stochK);
    j["stoch_d"] = optionalToJson(snapshot.stochD);
    j["bb_middle"] = optionalToJson(snapshot.bbMiddle);
    j["bb_upper"] = optionalToJson(snapshot.bbUpper);
    j["bb_lower"] = optionalToJson(snapshot.bbLower);
    j["atr"] = optionalToJson(snapshot.atr);
    j["vwap"] = optionalToJson(snapshot.vwap);
    j["obv"] = optionalToJson(snapshot.obv);
    return j;
}

json predictionToJson(const optional<PredictionSnapshot>& prediction) {
    if (!prediction.has_value()) {
        return json(nullptr);
    }
    json j;
    j["entry_long"] = optionalToJson(prediction->entryLong);
    j["entry_short"] = optionalToJson(prediction->entryShort);
    j["take_profit_long"] = optionalToJson(prediction->takeProfitLong);
    j["take_profit_short"] = optionalToJson(prediction->takeProfitShort);
    j["stop_loss_long"] = optionalToJson(prediction->stopLossLong);
    j["stop_loss_short"] = optionalToJson(prediction->stopLossShort);
    json zones = json::array();
    for (const auto& zone : prediction->zones) {
        json z;
        z["label"] = zone.label;
        z["price_high"] = zone.priceHigh;
        z["price_low"] = zone.priceLow;
        z["start_time_ms"] = zone.startTimeMs;
        z["end_time_ms"] = zone.endTimeMs.has_value() ? json(*zone.endTimeMs) : json(nullptr);
        zones.push_back(move(z));
    }
    j["zones"] = move(zones);
    return j;
}

// Every field a single candle contributes to the wire format, EXCEPT the
// envelope fields ("type"/"symbol"/"timeframe") that toJson() adds for a
// live message and toHistoryJson() below adds once for the whole batch
// instead of repeating per-candle - at up to 50,000 candles per timeframe,
// that repetition would meaningfully bloat the initial-connect payload for
// no benefit, since every candle in one history batch already shares the
// same symbol/timeframe.
json candleFieldsToJson(const VisualizationMessage& message) {
    json j;
    j["open_time"] = message.openTime;
    j["close_time"] = message.closeTime;
    j["closed"] = message.closed;
    j["open"] = message.open;
    j["high"] = message.high;
    j["low"] = message.low;
    j["close"] = message.close;
    j["base_volume"] = message.baseVolume;
    j["quote_volume"] = message.quoteVolume;
    j["taker_buy_base_volume"] = message.takerBuyBaseVolume;
    j["taker_buy_quote_volume"] = message.takerBuyQuoteVolume;
    j["indicators"] = indicatorSnapshotToJson(message.indicatorValues);
    j["prediction"] = predictionToJson(message.prediction);
    return j;
}

// Serializes an entire timeframe's ring buffer as ONE message instead of
// one message per candle. At up to 50,000 candles this is the difference
// between a handful of async_write calls and 50,000 of them, and between
// one bulk chart.setData() on the frontend and 50,000 individual
// series.update() calls - both meaningfully slower and, for the frontend,
// visibly janky if done one at a time.
string toHistoryJson(const string& symbol, const string& timeframe,
                     const map<int64_t, VisualizationMessage>& buffer) {
    json j;
    j["type"] = "history";
    j["symbol"] = symbol;
    j["timeframe"] = timeframe;
    json candles = json::array();
    for (const auto& entry : buffer) {
        candles.push_back(candleFieldsToJson(entry.second));
    }
    j["candles"] = move(candles);
    return j.dump();
}

// ---------------------------------------------------------------------------
// SMC/ICT JSON serialization. Every one of these mirrors a plain struct
// from smc-ict/smc_types.h field-for-field - see that header for what each
// field means; this is purely a naming transcription (camelCase struct
// field -> snake_case JSON key, matching indicatorSnapshotToJson()'s
// existing convention above).
// ---------------------------------------------------------------------------

json swingPointToJson(const smc::SwingPoint& sp) {
    json j;
    j["is_high"] = sp.isHigh;
    j["price"] = sp.price;
    j["open_time"] = sp.openTime;
    j["swept"] = sp.swept;
    j["swept_at"] = optionalMsToJson(sp.sweptAtOpenTime);
    return j;
}

json fvgToJson(const smc::FairValueGap& f) {
    json j;
    j["bullish"] = f.bullish;
    j["gap_low"] = f.gapLow;
    j["gap_high"] = f.gapHigh;
    j["ce"] = f.consequentEncroachment;
    j["formed_at"] = f.formedAtOpenTime;
    j["displacement_at"] = f.displacementOpenTime;
    j["confirmed_at"] = f.confirmedAtOpenTime;
    j["mitigated"] = f.mitigated;
    j["filled"] = f.filled;
    j["inverted"] = f.inverted;
    j["mitigated_at"] = optionalMsToJson(f.mitigatedAtOpenTime);
    j["inverted_at"] = optionalMsToJson(f.invertedAtOpenTime);
    return j;
}

json balancedRangeToJson(const smc::BalancedPriceRange& b) {
    json j;
    j["range_low"] = b.rangeLow;
    j["range_high"] = b.rangeHigh;
    j["formed_at"] = b.formedAtOpenTime;
    return j;
}

json orderBlockToJson(const smc::OrderBlock& ob) {
    json j;
    j["bullish"] = ob.bullish;
    j["subtype"] = smc::orderBlockSubtypeLabel(ob.subtype);
    j["zone_low"] = ob.zoneLow;
    j["zone_high"] = ob.zoneHigh;
    j["refined_low"] = ob.refinedLow;
    j["refined_high"] = ob.refinedHigh;
    j["mean_threshold"] = ob.meanThreshold;
    j["anchor_at"] = ob.anchorOpenTime;
    j["invalidated"] = ob.invalidated;
    j["mitigated"] = ob.mitigated;
    j["invalidated_at"] = optionalMsToJson(ob.invalidatedAtOpenTime);
    j["mitigated_at"] = optionalMsToJson(ob.mitigatedAtOpenTime);
    return j;
}

json structuralEventToJson(const smc::StructuralEvent& ev) {
    json j;
    j["kind"] = smc::structuralEventKindLabel(ev.kind);
    j["reference_price"] = ev.referencePrice;
    j["candle_close"] = ev.candleClose;
    j["open_time"] = ev.openTime;
    j["is_mss"] = ev.isMss;
    return j;
}

json equalLevelToJson(const smc::EqualLevel& eq) {
    json j;
    j["is_high"] = eq.isHigh;
    j["price_a"] = eq.priceA;
    j["price_b"] = eq.priceB;
    j["open_time_a"] = eq.openTimeA;
    j["open_time_b"] = eq.openTimeB;
    return j;
}

json premiumDiscountToJson(const optional<smc::PremiumDiscountZone>& zone) {
    if (!zone.has_value()) return json(nullptr);
    json j;
    j["swing_low"] = zone->swingLow;
    j["swing_high"] = zone->swingHigh;
    j["equilibrium"] = zone->equilibrium;
    j["ote_618"] = zone->ote618;
    j["ote_705"] = zone->ote705;
    j["ote_786"] = zone->ote786;
    j["bullish_leg"] = zone->bullishLeg;
    j["origin_at"] = zone->originOpenTime;
    j["terminus_at"] = zone->terminusOpenTime;
    return j;
}

json previousDayLevelsToJson(const optional<smc::PreviousDayLevels>& levels) {
    if (!levels.has_value()) return json(nullptr);
    json j;
    j["pdh"] = optionalToJson(levels->pdh);
    j["pdl"] = optionalToJson(levels->pdl);
    j["day_start_at"] = optionalMsToJson(levels->dayStartUtcMs);
    return j;
}

template <typename Container, typename ToJsonFn>
json arrayToJson(const Container& items, ToJsonFn toJsonFn) {
    json arr = json::array();
    for (const auto& item : items) {
        arr.push_back(toJsonFn(item));
    }
    return arr;
}

}  // namespace

string toJson(const VisualizationMessage& message) {
    json j = candleFieldsToJson(message);
    j["type"] = "candle";
    j["symbol"] = message.symbol;
    j["timeframe"] = message.timeframe;
    return j.dump();
}

string smcSnapshotToJson(const string& symbol, const string& timeframe,
                        const smc::SmcSnapshot& snapshot) {
    json j;
    j["type"] = "smc_snapshot";
    j["symbol"] = symbol;
    j["timeframe"] = timeframe;
    j["swing_highs"] = arrayToJson(snapshot.swingHighs, swingPointToJson);
    j["swing_lows"] = arrayToJson(snapshot.swingLows, swingPointToJson);
    j["fvgs"] = arrayToJson(snapshot.fvgs, fvgToJson);
    j["balanced_ranges"] = arrayToJson(snapshot.balancedRanges, balancedRangeToJson);
    j["order_blocks"] = arrayToJson(snapshot.orderBlocks, orderBlockToJson);
    j["equal_levels"] = arrayToJson(snapshot.equalLevels, equalLevelToJson);
    j["pdh"] = optionalToJson(snapshot.pdh);
    j["pdl"] = optionalToJson(snapshot.pdl);
    j["premium_discount"] = premiumDiscountToJson(snapshot.premiumDiscountZone);
    j["bias"] = smc::biasLabel(snapshot.bias);
    return j.dump();
}

string smcUpdateToJson(const string& symbol, const string& timeframe, const smc::SmcUpdate& update) {
    json j;
    j["type"] = "smc_update";
    j["symbol"] = symbol;
    j["timeframe"] = timeframe;
    j["swings"] = arrayToJson(update.swings, swingPointToJson);
    j["fvgs_formed"] = arrayToJson(update.fvgsFormed, fvgToJson);
    j["fvgs_changed"] = arrayToJson(update.fvgsChanged, fvgToJson);
    j["balanced_ranges_formed"] = arrayToJson(update.balancedRangesFormed, balancedRangeToJson);
    j["obs_formed"] = arrayToJson(update.obsFormed, orderBlockToJson);
    j["obs_changed"] = arrayToJson(update.obsChanged, orderBlockToJson);
    j["structural_events"] = arrayToJson(update.structuralEvents, structuralEventToJson);
    j["equal_levels"] = arrayToJson(update.equalLevels, equalLevelToJson);
    j["previous_day_levels"] = previousDayLevelsToJson(update.previousDayLevels);
    j["premium_discount"] = premiumDiscountToJson(update.premiumDiscountZone);
    j["kill_zone"] = update.killZone.has_value() ? json(smc::killZoneLabel(*update.killZone)) : json(nullptr);
    j["bias"] = smc::biasLabel(update.bias);
    return j.dump();
}

namespace {

// ---------------------------------------------------------------------------
// One WebSocket session - the single active viewer in this strictly
// single-viewer design. All methods here are only ever called from the
// VisualizationServer's one dedicated io_context thread (see Impl below),
// so - deliberately - there is no internal locking: the outgoing write
// queue is safe purely because nothing else ever touches it concurrently.
// ---------------------------------------------------------------------------
class Session : public enable_shared_from_this<Session> {
public:
    using ReadyHandler = function<void(const shared_ptr<Session>&)>;

    Session(tcp::socket socket, ReadyHandler onReady)
        : ws_(move(socket)), onReady_(move(onReady)) {}

    void run() {
        // Tightened from Beast's server-role defaults (300s idle / a ping
        // around the ~150s mark, keep_alive_pings already true by
        // default) to something snappier: this is a local, single-viewer
        // tool, so a stale session (e.g. a closed browser tab that never
        // sent a clean close frame) should be noticed and cleaned up in
        // seconds, not minutes. A 20s idle window is still far longer
        // than a loopback ping/pong round trip needs, so a healthy
        // connection is never at real risk of a spurious timeout.
        auto timeoutOpt = websocket::stream_base::timeout::suggested(beast::role_type::server);
        timeoutOpt.idle_timeout = seconds(20);
        timeoutOpt.keep_alive_pings = true;  // explicit, though already the server-role default
        ws_.set_option(timeoutOpt);
        ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
            res.set(http::field::server, "Traidle-Visualizer");
        }));
        ws_.async_accept(beast::bind_front_handler(&Session::onAccept, shared_from_this()));
    }

    // Enqueues one JSON payload for this session and kicks off writing if
    // idle. Never blocks - Beast forbids more than one async_write in
    // flight on the same stream at a time, so a second send() while a
    // write is already outstanding just queues behind it. A no-op once
    // the session is already dead_ (see close()) - no point queuing more
    // writes for a socket that's already gone.
    void send(string payload) {
        if (dead_) return;
        outgoing_.push_back(move(payload));
        if (!writing_) {
            doWrite();
        }
    }

    // Forcibly terminates this session's underlying TCP connection. Safe
    // to call more than once, and safe to call no matter what state the
    // connection's read/write sides are already in (e.g. mid-way through
    // processing a close frame the peer already sent, or having already
    // errored out on its own).
    //
    // This deliberately does NOT use Beast's higher-level
    // websocket::close()/async_close(). That API runs a cooperative
    // closing handshake with its own internal state machine, and it
    // asserts if that handshake is invoked while the stream is already
    // mid-close in some way (the exact crash this was hit by: "Assertion
    // failed: !impl.rd_close" in close.hpp). This method exists
    // specifically to FORCIBLY replace a session the instant a new one
    // takes its place (see Impl::startAccept()), and by definition there
    // is no guarantee about what state the old connection already found
    // itself in at that moment - closing the raw socket instead
    // (beast::close_socket, the documented low-level primitive for
    // exactly this) has no handshake and therefore nothing to assert
    // against. The browser still sees a normal disconnect (its
    // WebSocket's onclose fires either way) and reconnects exactly as it
    // would after a graceful close.
    void close() {
        if (dead_) return;
        dead_ = true;
        beast::close_socket(beast::get_lowest_layer(ws_));
    }

private:
    void onAccept(beast::error_code ec) {
        if (ec) return;  // handshake failed - this session is simply discarded
        if (onReady_) {
            onReady_(shared_from_this());
        }
        doRead();
    }

    void doRead() {
        // We don't expect anything meaningful from the browser; reading is
        // only here to detect a closed/broken connection promptly instead
        // of writing into a dead socket until a write eventually errors.
        ws_.async_read(readBuffer_, beast::bind_front_handler(&Session::onRead, shared_from_this()));
    }

    void onRead(beast::error_code ec, size_t) {
        if (ec) {
            dead_ = true;
            return;
        }
        readBuffer_.consume(readBuffer_.size());
        doRead();
    }

    void doWrite() {
        if (outgoing_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(outgoing_.front()),
                         beast::bind_front_handler(&Session::onWrite, shared_from_this()));
    }

    void onWrite(beast::error_code ec, size_t) {
        outgoing_.pop_front();
        if (ec) {
            dead_ = true;
            writing_ = false;
            return;
        }
        doWrite();
    }

    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer readBuffer_;
    deque<string> outgoing_;
    bool writing_ = false;
    bool dead_ = false;
    ReadyHandler onReady_;
};

}  // namespace

struct VisualizationServer::Impl {
    explicit Impl(unsigned short portIn) : port(portIn), acceptor(ioc), drainTimer(ioc) {}

    unsigned short port;
    net::io_context ioc;
    tcp::acceptor acceptor;
    net::steady_timer drainTimer;
    thread ioThread;
    bool running = false;

    shared_ptr<Session> activeSession;  // touched only from ioThread - see Session's class comment
    steady_clock::time_point lastSentAt = steady_clock::now();  // touched only from ioThread

    mutex queueMutex;
    deque<VisualizationMessage> outboundQueue;

    // Keyed by open time (not just appended in arrival order) so a
    // still-forming candle's repeated ticks overwrite the same slot
    // instead of piling up kHistoryDepth's worth of superseded
    // intermediate versions of what is really just one candle. A
    // std::map (not unordered_map) keeps each timeframe's buffer naturally
    // sorted by time, which sendHistorySnapshot() below relies on.
    mutex historyMutex;
    unordered_map<string, map<int64_t, VisualizationMessage>> historyByTimeframe;

    // SMC/ICT: current active-structure state, one entry per timeframe,
    // REPLACED wholesale (never appended/ring-buffered like candle history
    // above) - see visualization.h's class comment for why this is a
    // deliberately separate store rather than living inside
    // historyByTimeframe.
    mutex smcMutex;
    unordered_map<string, pair<string, smc::SmcSnapshot>> smcByTimeframe;  // timeframe -> (symbol, snapshot)

    // Live SMC/ICT event queue - same drop-oldest backpressure policy as
    // outboundQueue above, drained by the same timer (see scheduleDrain/
    // drainQueue), just kept as its own queue since smc::SmcUpdate is a
    // different shape from VisualizationMessage.
    mutex smcQueueMutex;
    deque<tuple<string, string, smc::SmcUpdate>> smcOutboundQueue;  // (symbol, timeframe, update)

    // Dedicated worker thread that builds the (potentially large) JSON
    // history-snapshot payloads - see sendHistorySnapshot()'s comment for
    // why this needs to happen off of ioThread entirely, kept alive for
    // the server's whole lifetime (started in start(), joined in stop())
    // rather than spawned fresh per connection.
    thread historyWorkerThread;
    mutex historyTaskMutex;
    condition_variable historyTaskCv;
    deque<function<void()>> historyTasks;
    bool historyWorkerRunning = false;

    void startAccept() {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = make_shared<Session>(move(socket), [this](const shared_ptr<Session>& ready) {
                    // Single-viewer: replace whatever was active only once
                    // the new connection has actually finished its
                    // handshake, so a failed/incomplete new connection
                    // never tears down a perfectly good existing one.
                    if (activeSession && activeSession != ready) {
                        activeSession->close();
                    }
                    activeSession = ready;
                    sendHistorySnapshot(ready);
                    lastSentAt = steady_clock::now();
                });
                session->run();
            }
            if (running) {
                startAccept();
            }
        });
    }

    // Sends each timeframe's entire ring buffer as one batched "history"
    // message (see toHistoryJson()), and each timeframe's current SMC/ICT
    // snapshot as one batched "smc_snapshot" message - both built the same
    // way, off ioThread, for the same reason (see the comment on
    // sendHistorySnapshot below, unchanged from before the SMC/ICT
    // additions: at up to 50,000 candles per timeframe, building this
    // JSON directly on ioThread would stall the accept loop, live-candle
    // broadcast, and heartbeats for the whole duration).
    //
    // This used to serialize directly on ioThread, which was fine back
    // when each timeframe's buffer capped at 500 candles. Now that the
    // cap is 50,000 (kHistoryDepth), a symbol with several active
    // timeframes near that cap can mean several hundred thousand candles
    // need serializing on every single new connection - tens to hundreds
    // of milliseconds of solid CPU work. Doing that directly on ioThread
    // would stall every OTHER pending operation there (the accept loop,
    // live-candle broadcast, heartbeats) for the whole duration - hurting
    // responsiveness on its own, and also widening the timing window in
    // which a rapid browser reconnect's session-replacement logic has to
    // reason about what state the old connection is already in.
    //
    // Only the already-built JSON payload strings get marshaled back onto
    // ioThread afterward (via net::post) - Session::send() and its
    // outgoing_ queue are only ever safe to touch from that one thread
    // (see Session's class comment), so the worker thread never touches
    // a Session directly, only through the io_context's own posting
    // mechanism. A weak_ptr is used (not shared_ptr) so that if this
    // session gets replaced again before its snapshot finishes building,
    // the now-stale task just quietly skips sending instead of doing
    // pointless work.
    void sendHistorySnapshot(const shared_ptr<Session>& session) {
        vector<pair<string, map<int64_t, VisualizationMessage>>> snapshot;
        {
            lock_guard<mutex> lock(historyMutex);
            snapshot.reserve(historyByTimeframe.size());
            for (const auto& entry : historyByTimeframe) {
                snapshot.emplace_back(entry.first, entry.second);
            }
        }

        vector<pair<string, pair<string, smc::SmcSnapshot>>> smcSnapshotCopy;
        {
            lock_guard<mutex> lock(smcMutex);
            smcSnapshotCopy.reserve(smcByTimeframe.size());
            for (const auto& entry : smcByTimeframe) {
                smcSnapshotCopy.emplace_back(entry.first, entry.second);
            }
        }

        weak_ptr<Session> weakSession = session;
        net::io_context* iocPtr = &ioc;
        {
            lock_guard<mutex> lock(historyTaskMutex);
            historyTasks.push_back([iocPtr, weakSession, snapshot = move(snapshot),
                                    smcSnapshotCopy = move(smcSnapshotCopy)]() mutable {
                for (auto& entry : snapshot) {
                    const auto& timeframe = entry.first;
                    const auto& buffer = entry.second;
                    if (buffer.empty()) continue;
                    const string& symbol = buffer.begin()->second.symbol;
                    string payload = toHistoryJson(symbol, timeframe, buffer);
                    net::post(*iocPtr, [weakSession, payload = move(payload)]() mutable {
                        if (auto session = weakSession.lock()) {
                            session->send(move(payload));
                        }
                    });
                }
                for (auto& entry : smcSnapshotCopy) {
                    const auto& timeframe = entry.first;
                    const auto& symbol = entry.second.first;
                    const auto& smcSnapshot = entry.second.second;
                    string payload = smcSnapshotToJson(symbol, timeframe, smcSnapshot);
                    net::post(*iocPtr, [weakSession, payload = move(payload)]() mutable {
                        if (auto session = weakSession.lock()) {
                            session->send(move(payload));
                        }
                    });
                }
            });
        }
        historyTaskCv.notify_one();
    }

    void historyWorkerLoop() {
        for (;;) {
            function<void()> task;
            {
                unique_lock<mutex> lock(historyTaskMutex);
                historyTaskCv.wait(lock, [this] { return !historyTasks.empty() || !historyWorkerRunning; });
                if (!historyWorkerRunning && historyTasks.empty()) {
                    return;
                }
                task = move(historyTasks.front());
                historyTasks.pop_front();
            }
            task();
        }
    }

    void scheduleDrain() {
        drainTimer.expires_after(milliseconds(5));
        drainTimer.async_wait([this](beast::error_code ec) {
            if (ec) return;
            drainQueue();
            drainSmcQueue();
            if (running) {
                scheduleDrain();
            }
        });
    }

    void drainQueue() {
        deque<VisualizationMessage> batch;
        {
            lock_guard<mutex> lock(queueMutex);
            batch.swap(outboundQueue);
        }
        if (!activeSession) {
            return;
        }
        if (!batch.empty()) {
            for (const auto& message : batch) {
                activeSession->send(toJson(message));
            }
            lastSentAt = steady_clock::now();
            return;
        }
        // Nothing real to send this tick - keep the link demonstrably
        // alive with a lightweight heartbeat once it's been quiet for a
        // while. Belt-and-suspenders alongside Beast's own keep-alive
        // pings: this also gives the frontend an explicit, low-latency
        // liveness signal independent of how often candles actually close.
        if (steady_clock::now() - lastSentAt > kHeartbeatInterval) {
            activeSession->send(R"({"type":"heartbeat"})");
            lastSentAt = steady_clock::now();
        }
    }

    void drainSmcQueue() {
        deque<tuple<string, string, smc::SmcUpdate>> batch;
        {
            lock_guard<mutex> lock(smcQueueMutex);
            batch.swap(smcOutboundQueue);
        }
        if (!activeSession || batch.empty()) {
            return;
        }
        for (const auto& entry : batch) {
            const auto& symbol = get<0>(entry);
            const auto& timeframe = get<1>(entry);
            const auto& update = get<2>(entry);
            activeSession->send(smcUpdateToJson(symbol, timeframe, update));
        }
        lastSentAt = steady_clock::now();
    }

    void recordHistory(const VisualizationMessage& message) {
        lock_guard<mutex> lock(historyMutex);
        auto& buffer = historyByTimeframe[message.timeframe];
        buffer[message.openTime] = message;  // insert or overwrite-in-place
        if (buffer.size() > kHistoryDepth) {
            buffer.erase(buffer.begin());  // std::map is sorted - this is the oldest open time
        }
    }
};

VisualizationServer::VisualizationServer(unsigned short port) : impl_(make_unique<Impl>(port)) {}

VisualizationServer::~VisualizationServer() { stop(); }

void VisualizationServer::start() {
    if (impl_->running) {
        return;
    }

    beast::error_code ec;
    // Bound to loopback only - this is a strictly single-viewer, local
    // visualization layer, not a service meant to be reachable from
    // anywhere else on the network.
    tcp::endpoint endpoint(net::ip::make_address("0.0.0.0"), impl_->port);
    impl_->acceptor.open(endpoint.protocol(), ec);
    if (!ec) impl_->acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if (!ec) impl_->acceptor.bind(endpoint, ec);
    if (!ec) impl_->acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        cerr << "[visualizer] failed to bind ws://127.0.0.1:" << impl_->port << " - " << ec.message()
             << ". Live charting will be unavailable; candle streaming/CSV/indicators are unaffected.\n";
        return;
    }

    impl_->running = true;
    impl_->historyWorkerRunning = true;
    impl_->startAccept();
    impl_->scheduleDrain();

    impl_->ioThread = thread([this]() {
        try {
            impl_->ioc.run();
        } catch (const exception& e) {
            cerr << "[visualizer] fatal error: " << e.what() << '\n';
        }
    });
    impl_->historyWorkerThread = thread([this]() { impl_->historyWorkerLoop(); });

    cout << "[visualizer] WebSocket server listening on ws://127.0.0.1:" << impl_->port
         << " - open web/visualizer.html in a browser to view live charts.\n";
}

void VisualizationServer::stop() {
    if (!impl_->running) {
        return;
    }
    impl_->running = false;
    impl_->ioc.stop();
    if (impl_->ioThread.joinable()) {
        impl_->ioThread.join();
    }
    {
        lock_guard<mutex> lock(impl_->historyTaskMutex);
        impl_->historyWorkerRunning = false;
    }
    impl_->historyTaskCv.notify_all();
    if (impl_->historyWorkerThread.joinable()) {
        impl_->historyWorkerThread.join();
    }
}

void VisualizationServer::push(VisualizationMessage message) {
    {
        lock_guard<mutex> lock(impl_->queueMutex);
        impl_->outboundQueue.push_back(message);
        if (impl_->outboundQueue.size() > kMaxQueueSize) {
            impl_->outboundQueue.pop_front();  // drop-oldest backpressure policy
        }
    }
    impl_->recordHistory(message);
}

void VisualizationServer::seedHistory(VisualizationMessage message) {
    impl_->recordHistory(message);
}

void VisualizationServer::setSmcSnapshot(const string& symbol, const string& timeframe,
                                        smc::SmcSnapshot snapshot) {
    lock_guard<mutex> lock(impl_->smcMutex);
    impl_->smcByTimeframe[timeframe] = {symbol, move(snapshot)};
}

void VisualizationServer::pushSmcEvent(const string& symbol, const string& timeframe,
                                      smc::SmcUpdate update) {
    lock_guard<mutex> lock(impl_->smcQueueMutex);
    impl_->smcOutboundQueue.emplace_back(symbol, timeframe, move(update));
    if (impl_->smcOutboundQueue.size() > kMaxSmcQueueSize) {
        impl_->smcOutboundQueue.pop_front();  // drop-oldest backpressure policy, same as outboundQueue
    }
}

}  // namespace visualization
