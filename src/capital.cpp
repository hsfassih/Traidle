#include "capital.h"

#include "candlesticks.h"
#include "capital_history.h"
#include "historical_get.h"
#include "indicators.h"
#include "smc-ict/smc_engine.h"
#include "visualization.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <thread>

namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using namespace std;
using namespace std::chrono;

namespace capital {

namespace {

// ---------------------------------------------------------------------------
// Tunables. Each one says why it has the value it has.
// ---------------------------------------------------------------------------

// Application-level ping interval. Cheap, proves the socket is alive even when
// no quotes flow (weekends), and each ping resets Capital's 10-minute session
// inactivity timer, so the tokens never expire while we are connected.
constexpr int kPingIntervalSeconds = 30;

// If NO frame at all (quote or ping reply) arrives for this long the connection
// is dead: close it and reconnect. Two and a half ping intervals, so one lost
// ping reply never trips it. Silence of QUOTES alone is NOT failure - forex is
// closed from Friday ~21:00 to Sunday ~22:00 UTC and quiet markets can go 17 s+.
constexpr int kWatchdogLimitSeconds = 75;
constexpr int kWatchdogCheckSeconds = 5;

// Reconnect backoff: 1, 2, 4, 8, 16, then 30 s forever. Never gives up (a feed
// that is down overnight must come back by itself) and there is no REST live
// fallback by design.
constexpr int kMaxBackoffSeconds = 30;

// Capital's session tokens die after 10 minutes without use. If we have heard
// nothing from the socket for longer than this, re-login before reconnecting
// instead of wasting an attempt on tokens that are certainly stale (8 min
// leaves a safety margin under the 10-minute limit).
constexpr auto kTokensCertainlyStaleAfter = minutes(8);

// The server-clock offset is re-measured this often (PC clocks drift; the
// owner's was ~3 s off Capital's).
constexpr auto kClockRefreshInterval = minutes(10);

// A forming candle is pushed to the visualizer / console at most this often per
// timeframe (plus immediately on the first tick of a new candle and right after a
// close). Binance updates about once a second per timeframe; at 4 Hz the chart
// still looks live, but a news burst of 50 quotes/s cannot flood the visualizer
// socket or the console mutex.
constexpr int64_t kPushThrottleNs = 250 * 1000 * 1000;

// How often a worker's timer runs: it closes candles of a quiet feed (serverNow
// >= bucketEnd + grace) and pushes a forming candle a throttled tick left stale.
constexpr auto kFlushInterval = milliseconds(500);

// Ticks held while a post-reconnect REST re-seed is in flight (it takes ~0.3-2 s;
// a news burst is a few hundred quotes). Beyond this the oldest information is
// already in the REST bar, so extra ticks are simply dropped and counted.
constexpr size_t kMaxHeldTicks = 50000;

// How many times the startup history pass is repeated when a candle closed
// between the pass and the REST seed (see runTimeframeWorker). Normally 1.
constexpr int kMaxStartupPasses = 3;

// Per-minute feed statistics line (quotes/min, WebSocket latency, worker
// hand-off latency). One scrolling line a minute; set false to silence it.
constexpr bool kLogFeedStats = true;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
int64_t steadyNowNs() {
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Runs a callable when it goes out of scope (C++17 has no std::scope_exit).
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(move(f)) {}
    ~ScopeExit() { f_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F f_;
};
template <class F>
ScopeExit<F> onScopeExit(F f) {
    return ScopeExit<F>(move(f));
}

string upperCopy(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(toupper(c)); });
    return value;
}

// Replaces every occurrence of a secret in `text` with "***". Frames the server
// sends should never contain our tokens, but nothing token-shaped may reach the
// console even if one does.
string redact(string text, const TokenSnapshot& tokens) {
    for (const string* secret : {&tokens.cst, &tokens.securityToken}) {
        if (secret->empty()) {
            continue;
        }
        size_t pos = 0;
        while ((pos = text.find(*secret, pos)) != string::npos) {
            text.replace(pos, secret->size(), "***");
            pos += 3;
        }
    }
    return text;
}

// ---------------------------------------------------------------------------
// Latency histogram: lock-free, ~19% resolution (4 buckets per octave), one
// instance per worker written only by that worker's thread and read (relaxed)
// by the feed's once-a-minute stats line. Recording costs one log2 and one
// relaxed atomic add - far below the 100 us per-tick budget.
// ---------------------------------------------------------------------------
constexpr int kHistogramBuckets = 160;  // 2^(160/4) ns ~ 1e12 ns: far beyond any real latency
using HistogramCounts = array<uint64_t, kHistogramBuckets>;

class LatencyHistogram {
public:
    LatencyHistogram() {
        for (auto& count : counts_) {
            count.store(0, memory_order_relaxed);
        }
    }
    void record(int64_t ns) {
        counts_[bucketFor(ns)].fetch_add(1, memory_order_relaxed);
    }
    void addTo(HistogramCounts& out) const {
        for (int i = 0; i < kHistogramBuckets; ++i) {
            out[i] += counts_[i].load(memory_order_relaxed);
        }
    }

    static int bucketFor(int64_t ns) {
        if (ns < 1) {
            return 0;
        }
        const int bucket = static_cast<int>(log2(static_cast<double>(ns)) * 4.0);
        return min(max(bucket, 0), kHistogramBuckets - 1);
    }
    static double valueNs(int bucket) { return exp2((bucket + 0.5) / 4.0); }

private:
    atomic<uint32_t> counts_[kHistogramBuckets];
};

// p in [0, 1]; returns nanoseconds (0 if the histogram is empty).
double histogramPercentileNs(const HistogramCounts& counts, double p) {
    uint64_t total = 0;
    for (const uint64_t c : counts) {
        total += c;
    }
    if (total == 0) {
        return 0;
    }
    const uint64_t rank = max<uint64_t>(1, static_cast<uint64_t>(ceil(p * static_cast<double>(total))));
    uint64_t seen = 0;
    for (int i = 0; i < kHistogramBuckets; ++i) {
        seen += counts[i];
        if (seen >= rank) {
            return LatencyHistogram::valueNs(i);
        }
    }
    return LatencyHistogram::valueNs(kHistogramBuckets - 1);
}

struct WorkerStats {
    LatencyHistogram handoff;  // feed receive -> start of the tick handler on the worker
    atomic<uint64_t> ticksHandled{0};
    atomic<uint64_t> lateDropped{0};
    atomic<uint64_t> partialSkipped{0};
    atomic<uint64_t> duplicateSkipped{0};
    atomic<uint64_t> closedWritten{0};
};

// ---------------------------------------------------------------------------
// StreamSession: one WebSocket connection to Capital's streaming endpoint. Same
// resolve -> connect -> TLS -> upgrade -> read-loop shape (and the same 15 s
// connect timeouts, suggested WebSocket timeouts and User-Agent) as
// KlineStreamSession in binance.cpp, with the Capital-specific parts:
//   - a single-writer outbound queue (Beast allows ONE async_write in flight),
//   - the ping / marketData.subscribe handshake and its acknowledgement,
//   - quotes arrive as JSON NUMBERS (Binance sends strings),
//   - an application-level ping every 30 s and a 75 s "nothing at all" watchdog.
// Every member is touched only from handlers on the stream's strand.
// ---------------------------------------------------------------------------
class StreamSession : public enable_shared_from_this<StreamSession> {
public:
    struct Callbacks {
        function<void(const Tick&)> onTick;
        function<void()> onActivity;  // any frame at all
        function<void()> onSubscribed;
        function<void(const string& /*step*/, beast::error_code)> onError;
        function<void(const string&)> onLog;
        function<void(const string&)> onFatal;  // configuration error: stop retrying
    };

    StreamSession(net::io_context& ioc, ssl::context& ctx, string host, string port, string epic,
                  TokenSnapshot tokens, Callbacks callbacks)
        : resolver_(net::make_strand(ioc)),
          ws_(net::make_strand(ioc), ctx),
          pingTimer_(ws_.get_executor()),
          watchdogTimer_(ws_.get_executor()),
          host_(move(host)),
          port_(move(port)),
          epic_(upperCopy(move(epic))),
          tokens_(move(tokens)),
          cb_(move(callbacks)) {
        hostHeader_ = port_ == "443" ? host_ : host_ + ":" + port_;
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
            throw beast::system_error{beast::error_code(
                static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
        }
        ws_.next_layer().set_verify_callback(ssl::host_name_verification(host_));
    }

    void run() {
        resolver_.async_resolve(host_, port_,
            beast::bind_front_handler(&StreamSession::onResolve, shared_from_this()));
    }

    // Intentional shutdown (no onError callback). Safe from any thread.
    void close() {
        net::post(ws_.get_executor(), [self = shared_from_this()] { self->shutdown(); });
    }

private:
    void onResolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) return fail("resolve", ec);
        beast::get_lowest_layer(ws_).expires_after(chrono::seconds(15));
        beast::get_lowest_layer(ws_).async_connect(results,
            beast::bind_front_handler(&StreamSession::onConnect, shared_from_this()));
    }

    void onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) return fail("tcp_connect", ec);
        beast::get_lowest_layer(ws_).expires_after(chrono::seconds(15));
        ws_.next_layer().async_handshake(ssl::stream_base::client,
            beast::bind_front_handler(&StreamSession::onSslHandshake, shared_from_this()));
    }

    void onSslHandshake(beast::error_code ec) {
        if (ec) return fail("tls_handshake", ec);
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "Traidle");
        }));
        ws_.async_handshake(hostHeader_, "/connect",
            beast::bind_front_handler(&StreamSession::onWsHandshake, shared_from_this()));
    }

    void onWsHandshake(beast::error_code ec) {
        if (ec) return fail("ws_upgrade", ec);
        lastActivity_ = steady_clock::now();
        // Ping first (cheap proof the session tokens are accepted), then subscribe.
        enqueue(makeFrame("ping", nullptr));
        json payload = {{"epics", json::array({epic_})}};
        enqueue(makeFrame("marketData.subscribe", &payload));
        armPing();
        armWatchdog();
        doRead();
    }

    // ---- outbound: single-writer queue ------------------------------------
    string makeFrame(const char* destination, const json* payload) {
        json frame;
        frame["destination"] = destination;
        frame["correlationId"] = to_string(++correlation_);
        frame["cst"] = tokens_.cst;
        frame["securityToken"] = tokens_.securityToken;
        if (payload != nullptr) {
            frame["payload"] = *payload;
        }
        return frame.dump();
    }

    void enqueue(string frame) {
        outbox_.push_back(move(frame));
        if (!writing_) {
            doWrite();
        }
    }

    void doWrite() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(outbox_.front()),
            beast::bind_front_handler(&StreamSession::onWrite, shared_from_this()));
    }

    void onWrite(beast::error_code ec, size_t) {
        if (ec) return fail("write", ec);
        outbox_.pop_front();
        if (!outbox_.empty()) {
            doWrite();
        } else {
            writing_ = false;
        }
    }

    // ---- timers -----------------------------------------------------------
    void armPing() {
        pingTimer_.expires_after(chrono::seconds(kPingIntervalSeconds));
        pingTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || self->done_) return;
            self->enqueue(self->makeFrame("ping", nullptr));
            self->armPing();
        });
    }

    void armWatchdog() {
        watchdogTimer_.expires_after(chrono::seconds(kWatchdogCheckSeconds));
        watchdogTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || self->done_) return;
            if (steady_clock::now() - self->lastActivity_ > chrono::seconds(kWatchdogLimitSeconds)) {
                self->cb_.onLog("no data of any kind for " + to_string(kWatchdogLimitSeconds) +
                                "s; treating the connection as dead");
                self->fail("watchdog", net::error::timed_out);
                return;
            }
            self->armWatchdog();
        });
    }

    // ---- inbound ----------------------------------------------------------
    void doRead() {
        ws_.async_read(buffer_,
            beast::bind_front_handler(&StreamSession::onRead, shared_from_this()));
    }

    void onRead(beast::error_code ec, size_t) {
        if (ec) return fail("read", ec);
        const int64_t receivedNs = steadyNowNs();
        lastActivity_ = steady_clock::now();
        if (cb_.onActivity) cb_.onActivity();
        // A parse or handler exception must never escape a read handler: it would
        // kill the connection. Log it, drop the frame, keep reading.
        try {
            const string text = beast::buffers_to_string(buffer_.data());
            buffer_.consume(buffer_.size());
            handleFrame(text, receivedNs);
        } catch (const exception& e) {
            buffer_.consume(buffer_.size());
            cb_.onLog(string("frame handling error: ") + e.what());
        }
        if (!done_) {
            doRead();
        }
    }

    void handleFrame(const string& text, int64_t receivedNs) {
        const json message = json::parse(text, nullptr, false);
        if (message.is_discarded() || !message.is_object()) {
            cb_.onLog("ignoring an unparseable frame");
            return;
        }
        const auto destinationIt = message.find("destination");
        const string destination = (destinationIt != message.end() && destinationIt->is_string())
                                       ? destinationIt->get<string>()
                                       : string();

        if (destination == "quote") {
            handleQuote(message, receivedNs);
            return;
        }

        const auto statusIt = message.find("status");
        const bool ok = statusIt != message.end() && statusIt->is_string() &&
                        statusIt->get<string>() == "OK";
        if (!ok) {
            const string safe = redact(text, tokens_);
            cb_.onLog("server replied with an error: " + safe);
            if (destination == "marketData.subscribe") {
                shutdown();
                if (cb_.onFatal) {
                    cb_.onFatal("Capital.com refused the subscription to '" + epic_ + "'");
                }
                return;
            }
            // Rejected tokens: reconnect with a fresh login rather than retry forever.
            string lower = safe;
            transform(lower.begin(), lower.end(), lower.begin(),
                      [](unsigned char c) { return static_cast<char>(tolower(c)); });
            if (lower.find("token") != string::npos || lower.find("session") != string::npos ||
                lower.find("unauthor") != string::npos || lower.find("auth") != string::npos) {
                fail("authentication", net::error::access_denied);
            }
            return;
        }

        if (destination == "marketData.subscribe") {
            bool processed = false;
            if (const auto payload = message.find("payload");
                payload != message.end() && payload->is_object()) {
                if (const auto subs = payload->find("subscriptions");
                    subs != payload->end() && subs->is_object()) {
                    if (const auto state = subs->find(epic_);
                        state != subs->end() && state->is_string()) {
                        processed = state->get<string>() == "PROCESSED";
                    }
                }
            }
            if (processed) {
                if (cb_.onSubscribed) cb_.onSubscribed();
            } else {
                shutdown();
                if (cb_.onFatal) {
                    cb_.onFatal("Capital.com did not accept the subscription to '" + epic_ +
                                "' (check the epic)");
                }
            }
        }
        // Ping replies and anything else: receiving them already refreshed lastActivity_.
    }

    // Capital sends numbers (not strings like Binance), so every field is checked
    // with is_number() and candlesticks::parseKlineMessage is deliberately NOT reused.
    void handleQuote(const json& message, int64_t receivedNs) {
        const auto payloadIt = message.find("payload");
        if (payloadIt == message.end() || !payloadIt->is_object()) {
            return;
        }
        const json& payload = *payloadIt;
        const auto epicIt = payload.find("epic");
        if (epicIt == payload.end() || !epicIt->is_string() || upperCopy(epicIt->get<string>()) != epic_) {
            return;
        }
        const auto bidIt = payload.find("bid");
        const auto timeIt = payload.find("timestamp");
        if (bidIt == payload.end() || !bidIt->is_number() || timeIt == payload.end() ||
            !timeIt->is_number()) {
            return;
        }
        Tick tick;
        tick.bid = bidIt->get<double>();
        const auto askIt = payload.find("ofr");
        tick.ask = (askIt != payload.end() && askIt->is_number()) ? askIt->get<double>() : 0.0;
        tick.tsMs = timeIt->is_number_integer() ? timeIt->get<int64_t>()
                                                : static_cast<int64_t>(llround(timeIt->get<double>()));
        tick.recvSteadyNs = receivedNs;
        if (cb_.onTick) cb_.onTick(tick);
    }

    // ---- teardown ---------------------------------------------------------
    void shutdown() {
        if (done_) return;
        done_ = true;
        pingTimer_.cancel();
        watchdogTimer_.cancel();
        beast::get_lowest_layer(ws_).close();
    }

    void fail(const string& what, beast::error_code ec) {
        if (done_) return;
        shutdown();
        if (cb_.onError) cb_.onError(what, ec);
    }

    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    net::steady_timer pingTimer_;
    net::steady_timer watchdogTimer_;
    beast::flat_buffer buffer_;
    string host_;
    string port_;
    string hostHeader_;
    string epic_;
    TokenSnapshot tokens_;
    Callbacks cb_;
    deque<string> outbox_;
    bool writing_ = false;
    bool done_ = false;
    uint64_t correlation_ = 0;
    steady_clock::time_point lastActivity_ = steady_clock::now();
};

// ---------------------------------------------------------------------------
// CapitalFeed: the one thread that owns the WebSocket. It owns login upkeep,
// keepalive, the watchdog and reconnect, and fans every quote out to the worker
// threads. Blocking work (re-login, clock refresh) runs on short-lived helper
// threads so the quote read loop is never stalled by REST.
// ---------------------------------------------------------------------------
class CapitalFeed {
public:
    CapitalFeed(shared_ptr<Session> session, string epic, pipeline::TerminalBoard& board)
        : session_(move(session)),
          epic_(move(epic)),
          board_(board),
          ctx_(ssl::context::tlsv12_client),
          reconnectTimer_(ioc_),
          clockTimer_(ioc_),
          statsTimer_(ioc_),
          sinks_(make_shared<const vector<shared_ptr<TickSink>>>()) {
        configureSslContext(ctx_, session_->endpoints());
        wsLatencyMs_.reserve(4096);
    }

    ~CapitalFeed() { stop(); }

    void start() {
        // The first handler is posted BEFORE the thread starts so run() has work.
        net::post(ioc_, [this] {
            lastActivity_ = steady_clock::now();
            connectNow();
            armClockTimer();
            armStatsTimer();
        });
        thread_ = thread([this] { ioc_.run(); });
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        if (thread_.joinable()) {
            net::post(ioc_, [this] {
                stopping_ = true;
                reconnectTimer_.cancel();
                clockTimer_.cancel();
                statsTimer_.cancel();
                if (current_) {
                    current_->close();
                }
                ioc_.stop();
            });
            thread_.join();
        }
        if (authThread_.joinable()) authThread_.join();
        if (clockThread_.joinable()) clockThread_.join();
    }

    // Sink registration is copy-on-write: the quote hot path never takes a lock
    // shared with registration, it just loads an immutable snapshot.
    void addSink(const shared_ptr<TickSink>& sink) {
        lock_guard<mutex> lock(registryMutex_);
        auto next = make_shared<vector<shared_ptr<TickSink>>>(*atomic_load(&sinks_));
        next->push_back(sink);
        atomic_store(&sinks_, shared_ptr<const vector<shared_ptr<TickSink>>>(next));
    }

    void removeSink(const shared_ptr<TickSink>& sink) {
        lock_guard<mutex> lock(registryMutex_);
        auto next = make_shared<vector<shared_ptr<TickSink>>>(*atomic_load(&sinks_));
        next->erase(remove(next->begin(), next->end(), sink), next->end());
        atomic_store(&sinks_, shared_ptr<const vector<shared_ptr<TickSink>>>(next));
    }

    void addStats(const shared_ptr<WorkerStats>& stats) {
        lock_guard<mutex> lock(registryMutex_);
        workerStats_.push_back(stats);
    }

private:
    // ---- connect / reconnect ---------------------------------------------
    void connectNow() {
        if (stopping_ || fatal_) return;
        const TokenSnapshot tokens = session_->tokens();
        string host = session_->streamingHost();
        string port = "443";
        if (const auto colon = host.rfind(':'); colon != string::npos) {
            port = host.substr(colon + 1);
            host.erase(colon);
        }

        const uint64_t id = ++sessionId_;
        StreamSession::Callbacks callbacks;
        callbacks.onTick = [this](const Tick& tick) { dispatch(tick); };
        callbacks.onActivity = [this] { lastActivity_ = steady_clock::now(); };
        callbacks.onSubscribed = [this, id] {
            if (id == sessionId_) onSubscribed();
        };
        callbacks.onError = [this, id](const string& step, beast::error_code ec) {
            if (id == sessionId_) onSessionError(step, ec.message());
        };
        callbacks.onLog = [this](const string& text) { board_.log("[capital] " + text); };
        callbacks.onFatal = [this, id](const string& text) {
            if (id != sessionId_) return;
            fatal_ = true;
            board_.log("[capital] fatal: " + text + "; the live feed has stopped (restart after fixing)");
        };

        try {
            current_ = make_shared<StreamSession>(ioc_, ctx_, host, port, epic_, tokens,
                                                  move(callbacks));
            current_->run();
        } catch (const exception& e) {
            board_.log(string("[capital] could not start the connection: ") + e.what());
            scheduleReconnect();
        }
    }

    void onSubscribed() {
        const bool firstTime = !everSubscribed_;
        everSubscribed_ = true;
        consecutiveFailures_ = 0;
        authSuspect_ = false;
        board_.log(string("[capital] ") + (firstTime ? "streaming " : "reconnected, streaming ") +
                   epic_);
        notifyReconnected(session_->serverNowMs());
    }

    void onSessionError(const string& step, const string& message) {
        if (step == "authentication") {
            // The server answered a frame with a token/session error: say that plainly
            // (the transport-level error text would only read "Permission denied").
            board_.log("[capital] the session tokens were rejected by the server; logging in again");
            authSuspect_ = true;
        } else {
            board_.log("[capital] " + step + " failed: " + message);
        }
        scheduleReconnect();
    }

    void scheduleReconnect() {
        if (stopping_ || fatal_) return;
        ++consecutiveFailures_;
        const int attempt = consecutiveFailures_;
        const int delaySeconds = attempt <= 5 ? (1 << (attempt - 1)) : kMaxBackoffSeconds;
        board_.log("[capital] reconnecting in " + to_string(delaySeconds) + "s (attempt " +
                   to_string(attempt) + ")");
        reconnectTimer_.expires_after(chrono::seconds(delaySeconds));
        reconnectTimer_.async_wait([this](beast::error_code ec) {
            if (ec) return;
            beginAttempt();
        });
    }

    void beginAttempt() {
        if (stopping_ || fatal_) return;
        // Re-login is only needed when the tokens are stale: after a long
        // silence, after the server rejected them, or - as a safety net - when
        // the second attempt in a row has failed.
        const bool stale = authSuspect_ || consecutiveFailures_ >= 2 ||
                           (steady_clock::now() - lastActivity_) > kTokensCertainlyStaleAfter;
        if (!stale) {
            connectNow();
            return;
        }
        authSuspect_ = false;
        if (authThread_.joinable()) {
            authThread_.join();  // the previous helper has already finished (we only run one at a time)
        }
        const uint64_t observed = session_->generation();
        authThread_ = thread([this, observed] {
            // Blocking REST (rate-limited login): never on the quote thread.
            string error;
            try {
                session_->refreshIfStale(observed);
            } catch (const exception& e) {
                error = e.what();
            }
            net::post(ioc_, [this, error] {
                if (stopping_) return;
                if (!error.empty()) {
                    board_.log("[capital] re-login failed: " + error);
                    scheduleReconnect();
                } else {
                    connectNow();
                }
            });
        });
    }

    // ---- fan-out ----------------------------------------------------------
    // Hot path: runs on the feed thread once per quote. One JSON parse already
    // happened; here it is one atomic snapshot load plus one post per worker.
    void dispatch(const Tick& tick) {
        ++quotesThisMinute_;
        if (wsLatencyMs_.size() < 100000) {
            wsLatencyMs_.push_back(session_->serverNowMs() - tick.tsMs);
        }
        const auto sinks = atomic_load(&sinks_);
        for (const auto& sink : *sinks) {
            lock_guard<mutex> lock(sink->m);  // contended only by that one worker at shutdown
            if (!sink->closed) {
                net::post(*sink->ioc, [sink, tick] { sink->onTick(tick); });
            }
        }
    }

    void notifyReconnected(int64_t connectedAtServerMs) {
        const auto sinks = atomic_load(&sinks_);
        for (const auto& sink : *sinks) {
            lock_guard<mutex> lock(sink->m);
            if (!sink->closed && sink->onReconnected) {
                net::post(*sink->ioc, [sink, connectedAtServerMs] { sink->onReconnected(connectedAtServerMs); });
            }
        }
    }

    // ---- housekeeping -----------------------------------------------------
    void armClockTimer() {
        clockTimer_.expires_after(kClockRefreshInterval);
        clockTimer_.async_wait([this](beast::error_code ec) {
            if (ec || stopping_) return;
            if (!clockBusy_.exchange(true)) {
                if (clockThread_.joinable()) {
                    clockThread_.join();
                }
                clockThread_ = thread([this] {
                    try {
                        session_->refreshClockOffset();
                    } catch (const exception&) {
                        // Keep the previous offset; try again next interval.
                    }
                    clockBusy_ = false;
                });
            }
            armClockTimer();
        });
    }

    void armStatsTimer() {
        statsTimer_.expires_after(chrono::seconds(60));
        statsTimer_.async_wait([this](beast::error_code ec) {
            if (ec || stopping_) return;
            emitStats();
            armStatsTimer();
        });
    }

    void emitStats() {
        vector<shared_ptr<WorkerStats>> workers;
        {
            lock_guard<mutex> lock(registryMutex_);
            workers = workerStats_;
        }
        const uint64_t quotes = quotesThisMinute_;
        quotesThisMinute_ = 0;
        vector<int64_t> latencies;
        latencies.swap(wsLatencyMs_);
        wsLatencyMs_.reserve(4096);

        if (!kLogFeedStats) {
            return;
        }
        if (quotes == 0) {
            ++silentMinutes_;
            if (silentMinutes_ == 1) {
                board_.log("[capital] no quotes in the last minute (market closed or quiet; the "
                           "socket is kept alive by pings)");
            } else if (silentMinutes_ % 30 == 0) {
                board_.log("[capital] still no quotes after " + to_string(silentMinutes_) + " minutes");
            }
            return;
        }
        if (silentMinutes_ > 0) {
            board_.log("[capital] quotes resumed");
            silentMinutes_ = 0;
        }

        sort(latencies.begin(), latencies.end());
        auto percentile = [&](double p) -> int64_t {
            if (latencies.empty()) return 0;
            // Nearest-rank percentile: the ceil(p*n)-th smallest value (1-based).
            const size_t rank = max<size_t>(1, static_cast<size_t>(ceil(p * static_cast<double>(latencies.size()))));
            return latencies[min(latencies.size(), rank) - 1];
        };

        HistogramCounts current{};
        uint64_t handled = 0, late = 0, partial = 0, duplicates = 0;
        for (const auto& worker : workers) {
            worker->handoff.addTo(current);
            handled += worker->ticksHandled.load(memory_order_relaxed);
            late += worker->lateDropped.load(memory_order_relaxed);
            partial += worker->partialSkipped.load(memory_order_relaxed);
            duplicates += worker->duplicateSkipped.load(memory_order_relaxed);
        }
        HistogramCounts delta{};
        for (int i = 0; i < kHistogramBuckets; ++i) {
            delta[i] = current[i] - previousHistogram_[i];
        }
        previousHistogram_ = current;
        const int64_t handoffP50us = llround(histogramPercentileNs(delta, 0.50) / 1000.0);
        const int64_t handoffP99us = llround(histogramPercentileNs(delta, 0.99) / 1000.0);

        board_.log("[capital] last 60s: quotes=" + to_string(quotes) + " ws_latency_ms p50=" +
                   to_string(percentile(0.50)) + " p99=" + to_string(percentile(0.99)) +
                   " | handoff_us p50=" + to_string(handoffP50us) + " p99=" + to_string(handoffP99us) +
                   " | ticks_handled=" + to_string(handled) + " late_dropped=" + to_string(late) +
                   " partial_skipped=" + to_string(partial) + " dup_skipped=" + to_string(duplicates));
    }

    shared_ptr<Session> session_;
    string epic_;
    pipeline::TerminalBoard& board_;

    net::io_context ioc_;
    ssl::context ctx_;
    net::steady_timer reconnectTimer_;
    net::steady_timer clockTimer_;
    net::steady_timer statsTimer_;
    thread thread_;
    thread authThread_;
    thread clockThread_;
    atomic<bool> clockBusy_{false};
    atomic<bool> stopped_{false};

    // Feed-thread-only state.
    shared_ptr<StreamSession> current_;
    uint64_t sessionId_ = 0;
    bool stopping_ = false;
    bool fatal_ = false;
    bool everSubscribed_ = false;
    bool authSuspect_ = false;
    int consecutiveFailures_ = 0;
    steady_clock::time_point lastActivity_ = steady_clock::now();
    uint64_t quotesThisMinute_ = 0;
    vector<int64_t> wsLatencyMs_;
    HistogramCounts previousHistogram_{};
    int silentMinutes_ = 0;

    // Worker registry: written rarely (under the mutex), read on the hot path through an atomic snapshot.
    mutex registryMutex_;
    shared_ptr<const vector<shared_ptr<TickSink>>> sinks_;
    vector<shared_ptr<WorkerStats>> workerStats_;
};

// ---------------------------------------------------------------------------
// StopControl: lets run() (tests, the probe) stop every worker's io_context.
// In normal operation nothing ever calls stopAll() and workers run until Ctrl+C.
// ---------------------------------------------------------------------------
class StopControl {
public:
    bool isStopped() {
        lock_guard<mutex> lock(m_);
        return stopped_;
    }
    void add(net::io_context* ioc) {
        lock_guard<mutex> lock(m_);
        if (stopped_) {
            ioc->stop();  // a stopped context makes run() return immediately
        } else {
            contexts_.push_back(ioc);
        }
    }
    void remove(net::io_context* ioc) {
        lock_guard<mutex> lock(m_);
        contexts_.erase(std::remove(contexts_.begin(), contexts_.end(), ioc), contexts_.end());
    }
    void stopAll() {
        lock_guard<mutex> lock(m_);
        stopped_ = true;
        for (auto* ioc : contexts_) {
            ioc->stop();
        }
    }

private:
    mutex m_;
    bool stopped_ = false;
    vector<net::io_context*> contexts_;
};

// ---------------------------------------------------------------------------
// One fully isolated worker per timeframe, mirroring binance::runTimeframeStream
// step for step: own io_context, IndicatorEngine, SmcEngine, CSV handle, REST
// client and backfill. A failure on one timeframe cannot affect another.
// ---------------------------------------------------------------------------
void runTimeframeWorker(shared_ptr<Session> session, CapitalFeed& feed, StopControl& stopControl,
                        const string& epic, pipeline::TimeframeTask task,
                        filesystem::path csvPath, pipeline::TerminalBoard& board,
                        visualization::VisualizationServer& vizServer) {
    const string label = task.rawTimeframe;
    const string& symbol = epic;
    try {
        const auto specOptional = candle_builder::intervalFromBinance(task.binanceInterval);
        if (!specOptional) {
            board.log("[" + label + "] unsupported interval '" + task.binanceInterval + "'");
            return;
        }
        const candle_builder::IntervalSpec spec = *specOptional;

        // Declared first so it is destroyed last: every guard below that touches
        // the io_context (sink close, helper-thread join) runs before it dies.
        net::io_context ioc;
        stopControl.add(&ioc);
        auto stopGuard = onScopeExit([&] { stopControl.remove(&ioc); });

        // One IndicatorEngine / SmcEngine for this timeframe's whole lifetime,
        // warmed up by the backfill and reused unmodified by the live path -
        // exactly as in the Binance worker (see the comments there).
        indicators::IndicatorEngine engine;
        smc::SmcEngine smcEngine;
        vector<candlesticks::Candlestick> smcReplayCandles;
        vector<optional<double>> smcReplayAtrs;

        RestClient rest(session);
        candle_builder::TickCandleBuilder builder(spec, epic);
        auto stats = make_shared<WorkerStats>();
        ofstream csv;
        int64_t lastWrittenOpenMs = INT64_MIN;  // belt and braces against a duplicate CSV row
        int64_t seedAsOfMs = INT64_MIN;         // ticks stamped before this are already in the seed/history
        int64_t liveSinceNs = INT64_MAX;        // hand-off latency is only meaningful once we run live
        int64_t lastPushNs = 0;
        int64_t lastPushedOpenMs = INT64_MIN;
        bool formingDirty = false;

        // ---- rendering ----------------------------------------------------
        auto pushForming = [&](const candlesticks::Candlestick& candle, bool force) {
            const int64_t nowNs = steadyNowNs();
            if (!force && nowNs - lastPushNs < kPushThrottleNs) {
                formingDirty = true;  // the 500 ms timer delivers it if no newer tick does
                return;
            }
            lastPushNs = nowNs;
            lastPushedOpenMs = candle.openTime;
            formingDirty = false;

            const size_t columns = pipeline::terminalColumns();
            string line = pipeline::formatCandleLine(symbol, candle, "FORMING", false);
            if (columns > 0 && (label.size() + 3 + line.size()) >= columns) {
                line = pipeline::formatCandleLine(symbol, candle, "FORMING", true);
            }
            board.updateLine(task.rowIndex, line);
            // engine.peek() is a read-only "what if this candle closed now" preview
            // (see indicators.h): the engine itself is never touched by a forming candle.
            vizServer.push(pipeline::buildVisualizationMessage(symbol, label, candle, engine.peek(candle)));
        };

        auto handleClosed = [&](const candlesticks::Candlestick& candle, bool partial) {
            lastPushNs = 0;  // the next forming candle is pushed immediately
            if (partial) {
                // Never observed from its first tick (cold start / lost connection):
                // its high/low/volume may be incomplete, so it must not reach the CSV,
                // the indicator engine or the SMC engine. It is still shown live.
                ++stats->partialSkipped;
                vizServer.push(pipeline::buildVisualizationMessage(symbol, label, candle, engine.peek(candle)));
                board.updateLine(task.rowIndex, "skipped an incomplete candle (feed started mid-candle)");
                return;
            }
            if (candle.openTime <= lastWrittenOpenMs) {
                ++stats->duplicateSkipped;
                return;
            }
            // Only CLOSED candles ever reach update(); forming ones use peek() only.
            const auto snapshot =
                pipeline::writeClosedCandleRow(csv, candle, engine, vizServer, symbol, label);
            lastWrittenOpenMs = candle.openTime;
            auto smcUpdate = smcEngine.update(candle, snapshot.atr);
            if (!smcUpdate.empty()) {
                vizServer.pushSmcEvent(symbol, label, smcUpdate);
                vizServer.setSmcSnapshot(symbol, label, smcEngine.currentSnapshot());
            }
            ++stats->closedWritten;
            board.updateLine(task.rowIndex, "last candle saved to CSV");
        };

        auto processTick = [&](const Tick& tick) {
            if (tick.tsMs < seedAsOfMs) {
                return;  // already covered by the REST seed / history
            }
            ++stats->ticksHandled;
            auto result = builder.onTick(tick.tsMs, tick.bid);
            if (result.droppedLate) {
                ++stats->lateDropped;
                return;
            }
            if (result.closed) {
                handleClosed(*result.closed, result.closedPartial);
            }
            if (result.forming) {
                const bool force = result.closed.has_value() || result.forming->openTime != lastPushedOpenMs;
                pushForming(*result.forming, force);
            }
        };

        // ---- re-seed after a reconnect (REST runs on a helper thread) -----
        bool reseeding = false;
        bool reseedAgain = false;
        vector<Tick> heldTicks;
        thread reseedThread;
        auto reseedGuard = onScopeExit([&] {
            if (reseedThread.joinable()) reseedThread.join();
        });

        function<void()> startReseed;
        auto finishReseed = [&](const capital_history::FormingSeed& seed) {
            if (reseedThread.joinable()) {
                reseedThread.join();  // it posted us as its last act
            }
            reseeding = false;
            using Status = capital_history::FormingSeed::Status;
            if (seed.status == Status::Seeded) {
                if (!builder.seed(seed.bar, seed.asOfMs)) {
                    builder.invalidate();  // its bucket was already closed here: do not reopen it
                }
                seedAsOfMs = max(seedAsOfMs, seed.asOfMs);
            } else if (seed.status == Status::EmptyBucket) {
                if (builder.hasCurrent()) {
                    builder.invalidate();
                } else {
                    builder.markCovered(seed.asOfMs);
                }
                seedAsOfMs = max(seedAsOfMs, seed.asOfMs);
            } else {
                builder.invalidate();
                board.log("[" + label + "] could not re-seed after reconnect (" + seed.error +
                          "); the current candle is marked incomplete and will not be written");
            }
            // Replay what arrived while REST was in flight (those stamped before the
            // seed's as-of time are dropped by processTick: the seed already has them).
            vector<Tick> replay;
            replay.swap(heldTicks);
            for (const Tick& held : replay) {
                processTick(held);
            }
            if (reseedAgain) {
                reseedAgain = false;
                startReseed();
            }
        };
        startReseed = [&]() {
            if (reseeding) {
                reseedAgain = true;
                return;
            }
            reseeding = true;
            heldTicks.clear();
            if (reseedThread.joinable()) {
                reseedThread.join();
            }
            reseedThread = thread([&] {
                capital_history::FormingSeed seed =
                    capital_history::fetchFormingSeed(*session, rest, epic, spec);
                net::post(ioc, [&finishReseed, seed = move(seed)] { finishReseed(seed); });
            });
        };

        // ---- register with the feed BEFORE any backfill --------------------
        // Quotes that arrive during a long backfill queue up on this worker's
        // io_context instead of being lost; processTick() discards the ones the
        // history/seed already cover.
        auto sink = make_shared<TickSink>();
        sink->ioc = &ioc;
        sink->onTick = [&](const Tick& tick) {
            const int64_t nowNs = steadyNowNs();
            if (tick.recvSteadyNs >= liveSinceNs) {
                stats->handoff.record(nowNs - tick.recvSteadyNs);
            }
            if (reseeding) {
                if (heldTicks.size() < kMaxHeldTicks) {
                    heldTicks.push_back(tick);
                }
                return;
            }
            processTick(tick);
        };
        sink->onReconnected = [&](int64_t connectedAtServerMs) {
            if (seedAsOfMs >= connectedAtServerMs) {
                return;  // we seeded after this connection came up: nothing was missed
            }
            startReseed();
        };
        feed.addSink(sink);
        feed.addStats(stats);
        auto sinkGuard = onScopeExit([&] {
            {
                lock_guard<mutex> lock(sink->m);
                sink->closed = true;  // the feed must never post to this io_context again
            }
            feed.removeSink(sink);
        });

        // ---- startup: history, SMC replay, seed ----------------------------
        board.updateLine(task.rowIndex, "checking historical data...");
        auto statusLine = [&](const string& msg) { board.updateLine(task.rowIndex, msg); };
        auto logLine = [&](const string& msg) { board.log("[" + label + "] " + msg); };
        auto seedHistory = [&](const candlesticks::Candlestick& candle,
                               const indicators::IndicatorSnapshot& snapshot) {
            // Seeds the visualizer's per-timeframe history ring buffer and the SMC
            // replay list from every replayed and freshly backfilled row.
            vizServer.seedHistory(pipeline::buildVisualizationMessage(symbol, label, candle, snapshot));
            smcReplayCandles.push_back(candle);
            smcReplayAtrs.push_back(snapshot.atr);
        };

        capital_history::FormingSeed seed;
        bool replayed = false;
        for (int pass = 0; pass < kMaxStartupPasses; ++pass) {
            const int64_t passStartMs = session->serverNowMs();
            const bool historyOk = capital_history::ensureContinuousHistory(
                *session, rest, epic, task, csvPath, statusLine, logLine, engine, seedHistory,
                /*replayExisting=*/!replayed);
            replayed = true;
            if (!historyOk && pass == 0) {
                board.log("[" + label +
                          "] historical backfill did not fully complete; continuing with live data "
                          "only (some earlier candles may still be missing).");
            }
            seed = capital_history::fetchFormingSeed(*session, rest, epic, spec);
            if (!historyOk) {
                break;
            }
            // The pass only wrote buckets that were safely closed when it started. If a
            // bucket ended between then and the seed's as-of time it is neither in the
            // CSV nor reachable by the live path: run one more (cheap) catch-up pass.
            const int64_t lastSafelyClosedMs = passStartMs - capital_history::kClosedSafetyMarginMs;
            if (candle_builder::bucketOpen(spec, seed.asOfMs) <=
                candle_builder::bucketOpen(spec, lastSafelyClosedMs)) {
                break;
            }
        }

        board.updateLine(task.rowIndex, "computing SMC/ICT structures...");
        smcEngine.replayHistory(smcReplayCandles, smcReplayAtrs);
        smcReplayCandles.clear();
        smcReplayCandles.shrink_to_fit();
        smcReplayAtrs.clear();
        smcReplayAtrs.shrink_to_fit();
        vizServer.setSmcSnapshot(symbol, label, smcEngine.currentSnapshot());

        csv = pipeline::openCsvAppend(csvPath);
        if (const auto stored = historical::lastStoredCsvOpenTimeMs(csvPath)) {
            lastWrittenOpenMs = *stored;
        }

        // Seed the forming candle so the first live candle is not truncated to
        // "ticks since launch". seedAsOfMs is set even when the REST call failed:
        // ticks stamped before it are history's business, not the live path's.
        seedAsOfMs = seed.asOfMs;
        using SeedStatus = capital_history::FormingSeed::Status;
        if (seed.status == SeedStatus::Seeded) {
            if (builder.seed(seed.bar, seed.asOfMs)) {
                if (auto forming = builder.peekForming()) {
                    pushForming(*forming, true);
                }
            }
        } else if (seed.status == SeedStatus::EmptyBucket) {
            builder.markCovered(seed.asOfMs);
            board.updateLine(task.rowIndex, "waiting for quotes (market closed or quiet)");
        } else {
            board.log("[" + label + "] could not seed the forming candle from REST (" + seed.error +
                      "); the first live candle will be shown but not written to the CSV");
        }

        // ---- live ----------------------------------------------------------
        net::steady_timer flushTimer(ioc);
        function<void(beast::error_code)> flushTick = [&](beast::error_code ec) {
            if (ec) return;
            if (!reseeding) {
                if (auto closed = builder.flush(session->serverNowMs())) {
                    handleClosed(*closed, builder.lastClosedPartial());
                } else if (formingDirty) {
                    if (auto forming = builder.peekForming()) {
                        pushForming(*forming, true);
                    }
                }
            }
            flushTimer.expires_after(kFlushInterval);
            flushTimer.async_wait(flushTick);
        };
        flushTimer.expires_after(kFlushInterval);
        flushTimer.async_wait(flushTick);

        liveSinceNs = steadyNowNs();
        if (!stopControl.isStopped()) {
            ioc.run();
        }
    } catch (const exception& e) {
        board.log("[" + label + "] Fatal error: " + string(e.what()));
    }
}

// ---------------------------------------------------------------------------
// preflight helpers
// ---------------------------------------------------------------------------
string upperEpic(const string& epic) { return upperCopy(epic); }

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
shared_ptr<Session> preflightWith(Credentials credentials, Endpoints endpoints, const string& epic,
                                  string& errorText) {
    if (!isValidEpic(epic)) {
        errorText = "Invalid epic '" + epic + "': use letters, digits, '.', '_' or '-' only.";
        return nullptr;
    }

    auto session = make_shared<Session>(move(credentials), move(endpoints));
    try {
        session->login();
    } catch (const exception& e) {
        errorText = e.what();  // already a human-readable explanation, never containing secrets
        return nullptr;
    }

    try {
        session->refreshClockOffset();
    } catch (const exception& e) {
        // Not fatal: candles are bucketed by quote timestamp, so the offset only
        // affects when a quiet candle is closed. Say so and carry on.
        cerr << "Warning: " << e.what() << " (using the local clock)." << endl;
    }

    try {
        RestClient rest(session);
        const HttpResult market = rest.get("/api/v1/markets/" + upperEpic(epic));
        if (market.status < 200 || market.status >= 300) {
            if (market.status == 404 || market.status == 400) {
                errorText = "Unknown epic '" + epic + "' on Capital.com";
            } else {
                errorText = "Capital.com could not look up epic '" + epic + "' (HTTP " +
                            to_string(market.status) + ")";
            }
            return nullptr;
        }
        // Informational only; balances and account details are never read or printed.
        string name;
        string marketStatus = "UNKNOWN";
        if (market.body.is_object()) {
            if (const auto instrument = market.body.find("instrument");
                instrument != market.body.end() && instrument->is_object()) {
                const auto n = instrument->find("name");
                if (n != instrument->end() && n->is_string()) name = n->get<string>();
            }
            if (const auto snapshot = market.body.find("snapshot");
                snapshot != market.body.end() && snapshot->is_object()) {
                const auto s = snapshot->find("marketStatus");
                if (s != snapshot->end() && s->is_string()) marketStatus = s->get<string>();
            }
        }
        cout << "Capital.com: " << (name.empty() ? upperEpic(epic) : name) << " (" << upperEpic(epic)
             << "), market status: " << marketStatus << endl;
        if (marketStatus != "TRADEABLE") {
            cout << "The market is not open right now: history still backfills, and live quotes "
                    "will start when it opens."
                 << endl;
        }
    } catch (const exception& e) {
        errorText = string("Capital.com epic check failed: ") + e.what();
        return nullptr;
    }
    return session;
}

shared_ptr<Session> preflight(const string& epic, string& errorText) {
    const auto credentials = credentialsFromEnv(errorText);
    if (!credentials) {
        return nullptr;
    }
    return preflightWith(*credentials, endpointsFor(credentials->demo), epic, errorText);
}

void run(shared_ptr<Session> session, const string& epic,
         const vector<pipeline::TimeframeTask>& tasks, const filesystem::path& folder,
         pipeline::TerminalBoard& board, visualization::VisualizationServer& vizServer,
         const RunOptions& options) {
    StopControl stopControl;
    CapitalFeed feed(session, upperEpic(epic), board);

    vector<thread> workers;
    workers.reserve(tasks.size());
    for (const auto& task : tasks) {
        filesystem::path csvPath = folder / (task.englishName + ".csv");
        workers.emplace_back(runTimeframeWorker, session, ref(feed), ref(stopControl), upperEpic(epic),
                             task, move(csvPath), ref(board), ref(vizServer));
    }
    feed.start();

    if (options.runFor) {
        this_thread::sleep_for(*options.runFor);
        stopControl.stopAll();
    }
    for (auto& worker : workers) {
        worker.join();
    }
    feed.stop();
}

}  // namespace capital