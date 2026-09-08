#include "visualization.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

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
constexpr size_t kHistoryDepth = 500;      // distinct candles retained per timeframe for snapshot-on-connect
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

}  // namespace

string toJson(const VisualizationMessage& message) {
    json j;
    j["type"] = "candle";
    j["symbol"] = message.symbol;
    j["timeframe"] = message.timeframe;
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
    // write is already outstanding just queues behind it.
    void send(string payload) {
        outgoing_.push_back(move(payload));
        if (!writing_) {
            doWrite();
        }
    }

    void close() {
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
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
        if (ec) return;  // client disconnected - session becomes inert, replaced on next connect
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
            writing_ = false;
            return;  // dead session - left in place until the next accepted connection replaces it
        }
        doWrite();
    }

    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer readBuffer_;
    deque<string> outgoing_;
    bool writing_ = false;
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
    // sorted by time, which sendHistorySnapshot() below relies on to
    // replay history to a freshly connected browser in chronological order.
    mutex historyMutex;
    unordered_map<string, map<int64_t, VisualizationMessage>> historyByTimeframe;

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

    void sendHistorySnapshot(const shared_ptr<Session>& session) {
        lock_guard<mutex> lock(historyMutex);
        for (const auto& [timeframe, buffer] : historyByTimeframe) {
            for (const auto& entry : buffer) {
                session->send(toJson(entry.second));
            }
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
    tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), impl_->port);
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
    impl_->startAccept();
    impl_->scheduleDrain();

    impl_->ioThread = thread([this]() {
        try {
            impl_->ioc.run();
        } catch (const exception& e) {
            cerr << "[visualizer] fatal error: " << e.what() << '\n';
        }
    });

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