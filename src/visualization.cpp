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
constexpr size_t kHistoryDepth = 50000;    // distinct candles retained per timeframe for snapshot-on-connect
constexpr auto kHeartbeatInterval = seconds(15);  // sent only if nothing real went out in this window

json optionalToJson(const optional<double>& value) {
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

}  // namespace

string toJson(const VisualizationMessage& message) {
    json j = candleFieldsToJson(message);
    j["type"] = "candle";
    j["symbol"] = message.symbol;
    j["timeframe"] = message.timeframe;
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
    // message (see toHistoryJson()). Copies the buffers out from under
    // historyMutex FIRST (cheap - copying already-built structs), then
    // hands the copies to the dedicated history worker thread to actually
    // build the JSON.
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

        weak_ptr<Session> weakSession = session;
        net::io_context* iocPtr = &ioc;
        {
            lock_guard<mutex> lock(historyTaskMutex);
            historyTasks.push_back([iocPtr, weakSession, snapshot = move(snapshot)]() mutable {
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
    // tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), impl_->port);
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

}  // namespace visualization