#pragma once

// ---------------------------------------------------------------------------
// MockCapitalServer: an in-process stand-in for Capital.com, for the offline
// integration test (tests/capital_mock_test.cpp). It exists because the real
// API needs live credentials; this lets the whole Capital path - login, clock
// offset, REST backfill, the WebSocket quote feed, reconnects, expired tokens,
// a silent market - run in CI against a server whose behaviour is known.
//
// What it speaks (all over TLS with a throw-away self-signed certificate for
// 127.0.0.1, which the client is pointed at through capital::Endpoints):
//   POST /api/v1/session            login (CST / X-SECURITY-TOKEN headers, streamingHost)
//   GET  /api/v1/time               {"serverTime": ms}
//   GET  /api/v1/markets/{epic}     instrument + marketStatus
//   GET  /api/v1/prices/{epic}      bid/ask OHLC bars, resolution/max/to parameters
//   WSS  /connect                   ping, marketData.subscribe, "quote" frames
//
// GROUND TRUTH. Every quote and every bar derives from one pure function of
// time: a bid price per 250 ms tick slot (a slow wave plus hash noise, rounded
// to 5 decimals), present only while the market is open (no ticks inside the
// configured closed windows). A bar is the fold of the ticks inside its bucket;
// its volume is the tick count. So a candle the client builds from the live
// stream and the bar this server returns over REST for the same bucket are
// derived from identical data, and the test can check them against each other
// and against an oracle that folds raw ticks directly.
//
// The server runs on a VIRTUAL CLOCK (real time + a shift). The test uses the
// shift to place "now" just before a chosen boundary (e.g. a Monday 00:00 UTC
// month start, where all 15 timeframes close at once) and to emulate a PC clock
// that is minutes or years off, which the client must handle through /time.
//
// It also plays the strict gatekeeper: it counts requests per second (the real
// limit is 10/s per user), logins per second (1/s per key), records every
// endpoint and WebSocket destination it is asked for, and answers 401 /
// WebSocket errors for tokens it has expired, so the test can prove the client
// never touches a trading endpoint and never exceeds the limits.
// ---------------------------------------------------------------------------

#include "../src/candle_builder.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mockcap {

using namespace std;
using json = nlohmann::json;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

constexpr int64_t kTickStepMs = 250;
constexpr int64_t kMinuteMs = 60 * 1000;
constexpr int64_t kDayMs = 24 * 3600 * 1000;

inline int64_t floorDivMs(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

inline int64_t realNowMs() {
    return chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch())
        .count();
}

inline uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Bid price at tick slot k (time = k * kTickStepMs). A pure function: any
// process, thread or year gets the same number.
inline double bidAtIndex(int64_t k) {
    const double slow = 0.02 * sin(static_cast<double>(k) / 4.0e5);
    const double noise = (static_cast<double>(mix64(static_cast<uint64_t>(k)) % 801) - 400.0) * 1e-6;
    return static_cast<double>(llround((1.10000 + slow + noise) * 1e5)) / 1e5;
}

struct ClosedWindow {
    int64_t fromMs = 0;  // inclusive
    int64_t toMs = 0;    // exclusive
};

struct OBar {
    int64_t openMs = 0;
    double o = 0, h = 0, l = 0, c = 0;
    int64_t count = 0;  // number of ticks == lastTradedVolume
};

class GroundTruth {
public:
    void setClosedWindows(vector<ClosedWindow> windows) { windows_ = move(windows); }

    bool isOpen(int64_t tsMs) const {
        for (const auto& w : windows_) {
            if (tsMs >= w.fromMs && tsMs < w.toMs) {
                return false;
            }
        }
        return true;
    }

    // Folds every tick with fromMs <= ts < toMs and ts <= limitMs. No caching:
    // this is also the test's independent oracle.
    optional<OBar> foldTicks(int64_t fromMs, int64_t toMs, int64_t limitMs) const {
        int64_t k = -floorDivMs(-fromMs, kTickStepMs);  // ceil(fromMs / step)
        OBar bar;
        bar.openMs = fromMs;
        for (;; ++k) {
            const int64_t ts = k * kTickStepMs;
            if (ts >= toMs || ts > limitMs) {
                break;
            }
            if (!isOpen(ts)) {
                continue;
            }
            const double bid = bidAtIndex(k);
            if (bar.count == 0) {
                bar.o = bar.h = bar.l = bar.c = bid;
            } else {
                bar.h = max(bar.h, bid);
                bar.l = min(bar.l, bid);
                bar.c = bid;
            }
            ++bar.count;
        }
        if (bar.count == 0) {
            return nullopt;
        }
        return bar;
    }

    // One-minute bar, cached once the minute has fully elapsed. The server builds
    // every coarser bar from these.
    optional<OBar> minuteBar(int64_t minuteOpenMs, int64_t limitMs) {
        const bool complete = limitMs >= minuteOpenMs + kMinuteMs - 1;
        if (complete) {
            lock_guard<mutex> lock(mutex_);
            const auto it = cache_.find(minuteOpenMs);
            if (it != cache_.end()) {
                return it->second;
            }
        }
        const optional<OBar> bar = foldTicks(minuteOpenMs, minuteOpenMs + kMinuteMs, limitMs);
        if (complete) {
            lock_guard<mutex> lock(mutex_);
            cache_.emplace(minuteOpenMs, bar);
        }
        return bar;
    }

    // Bar for the bucket [openMs, endMs): the fold of its minute bars, with the
    // still-forming tail cut at limitMs. Used by the REST endpoint.
    optional<OBar> restBar(int64_t openMs, int64_t endMs, int64_t limitMs) {
        optional<OBar> total;
        for (int64_t minute = openMs; minute < endMs && minute <= limitMs; minute += kMinuteMs) {
            const optional<OBar> mb = minuteBar(minute, limitMs);
            if (!mb) {
                continue;
            }
            if (!total) {
                total = *mb;
                total->openMs = openMs;
            } else {
                total->h = max(total->h, mb->h);
                total->l = min(total->l, mb->l);
                total->c = mb->c;
                total->count += mb->count;
            }
        }
        return total;
    }

private:
    vector<ClosedWindow> windows_;
    mutex mutex_;
    unordered_map<int64_t, optional<OBar>> cache_;
};

// How the server interprets the (UNVERIFIED on the real API) `to=` parameter.
enum class ToMode {
    UtcInclusive,       // bars with open <= to, `to` read as UTC
    UtcExclusive,       // bars with open <  to
    ShiftedInclusive,   // `to` is read as UTC+toShiftHours
    ShiftedExclusive,
    Ignored,            // `to` has no effect: always the latest bars
    Rejected,           // HTTP 400
    IgnoresMax          // `to` works but every page holds only ~10 bars
};

struct ResolutionInfo {
    const char* name;
    int64_t stepMs;
    int64_t depthMs;  // how far back the server keeps bars of this resolution
    bool weekly;
};

inline const vector<ResolutionInfo>& resolutions() {
    static const vector<ResolutionInfo> table = {
        {"MINUTE", 60 * 1000LL, 6 * 3600 * 1000LL, false},
        {"MINUTE_5", 5 * 60 * 1000LL, 5 * kDayMs, false},
        {"MINUTE_15", 15 * 60 * 1000LL, 3 * kDayMs, false},
        {"MINUTE_30", 30 * 60 * 1000LL, 4 * kDayMs, false},
        {"HOUR", 3600 * 1000LL, 10 * kDayMs, false},
        {"HOUR_4", 4 * 3600 * 1000LL, 30 * kDayMs, false},
        {"DAY", kDayMs, 90 * kDayMs, false},
        {"WEEK", 7 * kDayMs, 140 * kDayMs, true},
    };
    return table;
}

class MockCapitalServer {
public:
    struct Config {
        string apiKey = "mockKEY-abcdef0123456789-zyx";
        string identifier = "trader@example.com";
        string password = "Mock-Passw0rd-123";
        string epic = "EURUSD";
        string tempDir;               // where the public certificate (CA file) is written
        int64_t clockShiftMs = 0;     // server clock = real clock + shift
        ToMode toMode = ToMode::UtcInclusive;
        int toShiftHours = 0;
        vector<ClosedWindow> closedWindows;
        map<string, int64_t> alignShiftMs;  // resolution -> bar-boundary shift (alignment self-check test)
        bool refuseSubscribe = false;
        double sentinelBalance = 98765.43;  // appears in the login body; must never be printed
    };

    explicit MockCapitalServer(Config config)
        : cfg_(move(config)),
          clockShiftMs_(cfg_.clockShiftMs),
          sslCtx_(ssl::context::tls_server),
          acceptor_(acceptIoc_) {
        truth_.setClosedWindows(cfg_.closedWindows);
        toMode_.store(static_cast<int>(cfg_.toMode));
        toShiftHours_.store(cfg_.toShiftHours);
        refuseSubscribe_.store(cfg_.refuseSubscribe);
        alignShiftMs_ = cfg_.alignShiftMs;
    }

    ~MockCapitalServer() { stop(); }

    void start() {
        string certPem, keyPem;
        makeSelfSigned(certPem, keyPem);
        caFile_ = cfg_.tempDir + "/mock_capital_ca.pem";
        {
            ofstream out(caFile_, ios::binary | ios::trunc);
            out << certPem;  // public certificate only
        }
        sslCtx_.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                            ssl::context::no_sslv3);
        sslCtx_.use_certificate_chain(net::buffer(certPem));
        sslCtx_.use_private_key(net::buffer(keyPem), ssl::context::pem);

        const tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), 0);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        port_ = acceptor_.local_endpoint().port();
        acceptThread_ = thread([this] { acceptLoop(); });
    }

    void stop() {
        if (stopping_.exchange(true)) {
            return;
        }
        if (acceptThread_.joinable()) {
            // Wake the blocking accept() with a throw-away connection.
            try {
                net::io_context ioc;
                tcp::socket wake(ioc);
                wake.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port_));
            } catch (...) {
            }
            acceptThread_.join();
        }
        vector<shared_ptr<Conn>> conns;
        {
            lock_guard<mutex> lock(connsMutex_);
            conns = conns_;
        }
        for (const auto& conn : conns) {
            conn->kill();
        }
        for (const auto& conn : conns) {
            if (conn->worker.joinable()) {
                conn->worker.join();
            }
        }
    }

    unsigned short port() const { return port_; }
    string caFile() const { return caFile_; }
    const Config& config() const { return cfg_; }
    GroundTruth& truth() { return truth_; }

    int64_t serverNowMs() const { return realNowMs() + clockShiftMs_.load(); }
    void setClockShiftMs(int64_t shift) { clockShiftMs_.store(shift); }

    // ---- fault injection -----------------------------------------------
    void expireTokens() {
        lock_guard<mutex> lock(stateMutex_);
        validTokens_.clear();
    }
    void dropWebSockets() { ++wsEpoch_; }
    // The next `count` REST requests (any endpoint but /time) are answered 429 with Retry-After: 1.
    void inject429(int count) { inject429_.store(count); }
    void setRefuseSubscribe(bool refuse) { refuseSubscribe_.store(refuse); }
    void setToMode(ToMode mode, int shiftHours = 0) {
        toMode_.store(static_cast<int>(mode));
        toShiftHours_.store(shiftHours);
    }
    void setAlignShift(const string& resolution, int64_t shiftMs) {
        lock_guard<mutex> lock(stateMutex_);
        alignShiftMs_[resolution] = shiftMs;
    }

    // ---- observations --------------------------------------------------
    int logins() const { return logins_.load(); }
    int wsConnections() const { return wsConnections_.load(); }
    int rateViolations() const { return rateViolations_.load(); }
    int loginViolations() const { return loginViolations_.load(); }
    int unauthorized() const { return unauthorized_.load(); }
    int quotesSent() const { return quotesSent_.load(); }
    int pings() const { return pings_.load(); }
    int maxRequestsInAnySecond() const { return maxInSecond_.load(); }
    // The requests (offset in ms from the oldest one, method and path) that made up the busiest
    // one-second window seen so far - for diagnosing a rate-limit assertion.
    string peakWindow() const {
        lock_guard<mutex> lock(stateMutex_);
        return peakWindow_;
    }
    vector<string> requestLog() const {
        lock_guard<mutex> lock(stateMutex_);
        return requestLog_;
    }
    set<string> wsDestinations() const {
        lock_guard<mutex> lock(stateMutex_);
        return wsDestinations_;
    }
    int pricesRequests() const {
        lock_guard<mutex> lock(stateMutex_);
        int n = 0;
        for (const auto& r : requestLog_) {
            if (r.rfind("GET /api/v1/prices/", 0) == 0) ++n;
        }
        return n;
    }

private:
    struct Conn {
        shared_ptr<net::io_context> ioc;
        unique_ptr<ssl::stream<tcp::socket>> tls;
        int fd = -1;
        bool alive = true;
        mutex m;
        thread worker;
        void kill() {
            lock_guard<mutex> lock(m);
            if (alive && fd >= 0) {
                // shutdown() (not close) so a thread blocked in a read wakes up
                // without the descriptor being recycled under it.
                ::shutdown(fd, 2);
            }
        }
    };

    // ---- TLS ----------------------------------------------------------------
    static void makeSelfSigned(string& certPem, string& keyPem) {
        EVP_PKEY* pkey = nullptr;
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EVP_PKEY_keygen_init(kctx);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1);
        EVP_PKEY_keygen(kctx, &pkey);
        EVP_PKEY_CTX_free(kctx);

        X509* x = X509_new();
        X509_set_version(x, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
        X509_gmtime_adj(X509_getm_notBefore(x), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x), 3600 * 24 * 2);
        X509_set_pubkey(x, pkey);
        X509_NAME* name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x, name);

        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
        for (const auto& spec : {pair<int, const char*>{NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"},
                                 pair<int, const char*>{NID_basic_constraints, "critical,CA:TRUE"}}) {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, spec.first, const_cast<char*>(spec.second));
            X509_add_ext(x, ext, -1);
            X509_EXTENSION_free(ext);
        }
        X509_sign(x, pkey, EVP_sha256());

        auto toString = [](BIO* bio) {
            BUF_MEM* mem = nullptr;
            BIO_get_mem_ptr(bio, &mem);
            return string(mem->data, mem->length);
        };
        BIO* certBio = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(certBio, x);
        certPem = toString(certBio);
        BIO_free(certBio);
        BIO* keyBio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        keyPem = toString(keyBio);
        BIO_free(keyBio);

        X509_free(x);
        EVP_PKEY_free(pkey);
    }

    // ---- connections -----------------------------------------------------------
    void acceptLoop() {
        while (!stopping_) {
            auto ioc = make_shared<net::io_context>();
            tcp::socket socket(*ioc);
            boost::system::error_code ec;
            acceptor_.accept(socket, ec);
            if (stopping_) {
                break;
            }
            if (ec) {
                continue;
            }
            auto conn = make_shared<Conn>();
            conn->ioc = ioc;
            conn->fd = static_cast<int>(socket.native_handle());
            conn->tls = make_unique<ssl::stream<tcp::socket>>(move(socket), sslCtx_);
            {
                lock_guard<mutex> lock(connsMutex_);
                conns_.push_back(conn);
            }
            conn->worker = thread([this, conn] { serve(conn); });
        }
    }

    void serve(const shared_ptr<Conn>& conn) {
        try {
            boost::system::error_code ec;
            conn->tls->handshake(ssl::stream_base::server, ec);
            if (!ec) {
                beast::flat_buffer buffer;
                for (;;) {
                    http::request<http::string_body> req;
                    http::read(*conn->tls, buffer, req, ec);
                    if (ec) {
                        break;
                    }
                    if (websocket::is_upgrade(req)) {
                        serveWebSocket(conn, move(req));
                        break;
                    }
                    http::response<http::string_body> res = handleRest(req);
                    res.keep_alive(req.keep_alive());
                    res.prepare_payload();
                    http::write(*conn->tls, res, ec);
                    if (ec || !res.keep_alive()) {
                        break;
                    }
                }
            }
        } catch (...) {
        }
        lock_guard<mutex> lock(conn->m);
        conn->alive = false;
    }

    // ---- WebSocket --------------------------------------------------------------
    class WsSession : public enable_shared_from_this<WsSession> {
    public:
        WsSession(MockCapitalServer& server, shared_ptr<Conn> conn)
            : server_(server),
              conn_(move(conn)),
              ws_(move(*conn_->tls)),
              timer_(*conn_->ioc),
              epochAtStart_(server.wsEpoch_.load()) {}

        void run(http::request<http::string_body> req) {
            boost::system::error_code ec;
            ws_.accept(req, ec);
            if (ec) {
                return;
            }
            ++server_.wsConnections_;
            doRead();
            armTimer();
            conn_->ioc->run();
        }

    private:
        void doRead() {
            ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, size_t) {
                if (ec) {
                    self->timer_.cancel();
                    return;
                }
                const string text = beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());
                self->onFrame(text);
                self->doRead();
            });
        }

        void onFrame(const string& text) {
            const json frame = json::parse(text, nullptr, false);
            if (!frame.is_object()) {
                return;
            }
            const string destination = frame.value("destination", string());
            const string correlation = frame.value("correlationId", string());
            {
                lock_guard<mutex> lock(server_.stateMutex_);
                server_.wsDestinations_.insert(destination);
            }
            const bool authorized = server_.tokensValid(frame.value("cst", string()),
                                                        frame.value("securityToken", string()));
            if (!authorized) {
                reply({{"status", "ERROR"},
                       {"destination", destination},
                       {"correlationId", correlation},
                       {"payload", {{"errorCode", "error.invalid.session.token"}}}});
                return;
            }
            if (destination == "ping") {
                ++server_.pings_;
                reply({{"status", "OK"}, {"destination", "ping"}, {"correlationId", correlation},
                       {"payload", json::object()}});
            } else if (destination == "marketData.subscribe") {
                string epic;
                if (frame.contains("payload") && frame["payload"].contains("epics") &&
                    frame["payload"]["epics"].is_array() && !frame["payload"]["epics"].empty()) {
                    epic = frame["payload"]["epics"][0].get<string>();
                }
                const bool ok = !server_.refuseSubscribe_.load() && epic == server_.cfg_.epic;
                json subs = json::object();
                subs[epic] = ok ? "PROCESSED" : "NOT_PROCESSED";
                reply({{"status", "OK"}, {"destination", "marketData.subscribe"},
                       {"correlationId", correlation}, {"payload", {{"subscriptions", subs}}}});
                if (ok) {
                    subscribed_ = true;
                    nextTick_ = floorDivMs(server_.serverNowMs(), kTickStepMs) + 1;
                }
            } else {
                reply({{"status", "ERROR"}, {"destination", destination}, {"correlationId", correlation},
                       {"payload", {{"errorCode", "error.unknown.destination"}}}});
            }
        }

        void armTimer() {
            timer_.expires_after(chrono::milliseconds(40));
            timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    return;
                }
                if (self->server_.stopping_ || self->server_.wsEpoch_.load() != self->epochAtStart_) {
                    // Abrupt drop: no close frame, like a network failure.
                    ::shutdown(self->conn_->fd, 2);
                    return;
                }
                self->emitTicks();
                self->armTimer();
            });
        }

        void emitTicks() {
            if (!subscribed_) {
                return;
            }
            const int64_t now = server_.serverNowMs();
            int64_t last = floorDivMs(now, kTickStepMs);
            if (last - nextTick_ > 5 * 1000 / kTickStepMs) {
                nextTick_ = last - 5 * 1000 / kTickStepMs;  // never replay a long backlog
            }
            for (; nextTick_ <= last; ++nextTick_) {
                const int64_t ts = nextTick_ * kTickStepMs;
                if (!server_.truth_.isOpen(ts)) {
                    continue;
                }
                const double bid = bidAtIndex(nextTick_);
                json frame = {{"status", "OK"},
                              {"destination", "quote"},
                              {"payload",
                               {{"product", "CFD"},
                                {"epic", server_.cfg_.epic},
                                {"bid", bid},
                                {"bidQty", 1000000.0},
                                {"ofr", bid + 0.0001},
                                {"ofrQty", 1000000.0},
                                {"timestamp", ts}}}};
                outbox_.push_back(frame.dump());
                ++server_.quotesSent_;
            }
            if (!writing_ && !outbox_.empty()) {
                doWrite();
            }
        }

        void reply(const json& frame) {
            outbox_.push_back(frame.dump());
            if (!writing_) {
                doWrite();
            }
        }

        void doWrite() {
            writing_ = true;
            ws_.text(true);
            ws_.async_write(net::buffer(outbox_.front()),
                            [self = shared_from_this()](beast::error_code ec, size_t) {
                                if (ec) {
                                    self->timer_.cancel();
                                    return;
                                }
                                self->outbox_.pop_front();
                                if (!self->outbox_.empty()) {
                                    self->doWrite();
                                } else {
                                    self->writing_ = false;
                                }
                            });
        }

        MockCapitalServer& server_;
        shared_ptr<Conn> conn_;
        websocket::stream<ssl::stream<tcp::socket>> ws_;
        net::steady_timer timer_;
        beast::flat_buffer buffer_;
        deque<string> outbox_;
        bool writing_ = false;
        bool subscribed_ = false;
        int64_t nextTick_ = 0;
        uint64_t epochAtStart_;
    };

    void serveWebSocket(const shared_ptr<Conn>& conn, http::request<http::string_body> req) {
        // The session takes over the TLS stream. A WebSocket connection is
        // always the stream's last use, so the thread ends when it returns.
        auto session = make_shared<WsSession>(*this, conn);
        session->run(move(req));
    }

    // ---- REST ---------------------------------------------------------------------
    bool tokensValid(const string& cst, const string& security) const {
        lock_guard<mutex> lock(stateMutex_);
        return validTokens_.count(cst + "|" + security) > 0;
    }

    static string decodeQueryValue(const string& raw) {
        string out;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '%' && i + 2 < raw.size()) {
                out.push_back(static_cast<char>(stoi(raw.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }

    static map<string, string> parseQuery(const string& query) {
        map<string, string> out;
        size_t pos = 0;
        while (pos < query.size()) {
            size_t amp = query.find('&', pos);
            if (amp == string::npos) amp = query.size();
            const string pair = query.substr(pos, amp - pos);
            const size_t eq = pair.find('=');
            if (eq != string::npos) {
                out[pair.substr(0, eq)] = decodeQueryValue(pair.substr(eq + 1));
            }
            pos = amp + 1;
        }
        return out;
    }

    http::response<http::string_body> jsonResponse(http::status status, const json& body) const {
        http::response<http::string_body> res{status, 11};
        res.set(http::field::content_type, "application/json");
        res.body() = body.dump();
        return res;
    }

    // Counts the request against the per-second limit; true if it must be refused (429).
    bool overRateLimit(const string& label) {
        const auto now = chrono::steady_clock::now();
        lock_guard<mutex> lock(stateMutex_);
        requestTimes_.push_back(now);
        requestLabels_.push_back(label);
        while (!requestTimes_.empty() && now - requestTimes_.front() > chrono::seconds(1)) {
            requestTimes_.pop_front();
            requestLabels_.pop_front();
        }
        const int inWindow = static_cast<int>(requestTimes_.size());
        int previous = maxInSecond_.load();
        if (inWindow > previous) {
            maxInSecond_.store(inWindow);
            peakWindow_.clear();
            for (size_t i = 0; i < requestTimes_.size(); ++i) {
                const auto offset = chrono::duration_cast<chrono::milliseconds>(requestTimes_[i] - requestTimes_.front());
                peakWindow_ += "+" + to_string(offset.count()) + "ms " + requestLabels_[i].substr(0, 60) + "; ";
            }
        }
        if (inWindow > 10) {
            ++rateViolations_;
            return true;
        }
        return false;
    }

    http::response<http::string_body> handleRest(const http::request<http::string_body>& req) {
        const string target = string(req.target());
        const size_t qmark = target.find('?');
        const string path = target.substr(0, qmark);
        const string query = qmark == string::npos ? string() : target.substr(qmark + 1);
        {
            lock_guard<mutex> lock(stateMutex_);
            requestLog_.push_back(string(req.method_string()) + " " + target);
        }
        if (overRateLimit(string(req.method_string()) + " " + target)) {
            auto res = jsonResponse(http::status::too_many_requests,
                                    {{"errorCode", "error.too-many.requests"}});
            res.set(http::field::retry_after, "1");
            return res;
        }

        if (path != "/api/v1/time" && path != "/api/v1/session") {
            int pending = inject429_.load();
            if (pending > 0 && inject429_.compare_exchange_strong(pending, pending - 1)) {
                auto res = jsonResponse(http::status::too_many_requests,
                                        {{"errorCode", "error.too-many.requests"}});
                res.set(http::field::retry_after, "1");
                return res;
            }
        }
        if (path == "/api/v1/session" && req.method() == http::verb::post) {
            return handleLogin(req);
        }
        if (path == "/api/v1/time" && req.method() == http::verb::get) {
            return jsonResponse(http::status::ok, {{"serverTime", serverNowMs()}});
        }

        // Everything else needs valid tokens.
        if (!tokensValid(string(req["CST"]), string(req["X-SECURITY-TOKEN"]))) {
            ++unauthorized_;
            return jsonResponse(http::status::unauthorized, {{"errorCode", "error.invalid.session.token"}});
        }
        if (req.method() == http::verb::get && path.rfind("/api/v1/markets/", 0) == 0) {
            const string epic = path.substr(string("/api/v1/markets/").size());
            if (epic != cfg_.epic) {
                return jsonResponse(http::status::not_found, {{"errorCode", "error.prices.not-found"}});
            }
            return jsonResponse(http::status::ok,
                                {{"instrument", {{"epic", epic}, {"name", "EUR/USD"}, {"type", "CURRENCIES"}}},
                                 {"snapshot", {{"marketStatus", "TRADEABLE"}, {"bid", 1.1}, {"offer", 1.1001}}}});
        }
        if (req.method() == http::verb::get && path.rfind("/api/v1/prices/", 0) == 0) {
            return handlePrices(path.substr(string("/api/v1/prices/").size()), parseQuery(query));
        }
        // Anything else (positions, orders, accounts, confirms, ...) is not part of the
        // market-data surface: the real server would answer, the test fails on the log.
        return jsonResponse(http::status::not_found, {{"errorCode", "error.not-found"}});
    }

    http::response<http::string_body> handleLogin(const http::request<http::string_body>& req) {
        const auto now = chrono::steady_clock::now();
        {
            lock_guard<mutex> lock(stateMutex_);
            if (lastLogin_ && now - *lastLogin_ < chrono::milliseconds(1000)) {
                ++loginViolations_;
                lastLogin_ = now;
                return jsonResponse(http::status::too_many_requests,
                                    {{"errorCode", "error.too-many.requests"}});
            }
            lastLogin_ = now;
        }
        if (string(req["X-CAP-API-KEY"]) != cfg_.apiKey) {
            return jsonResponse(http::status::unauthorized, {{"errorCode", "error.invalid.api.key"}});
        }
        const json body = json::parse(req.body(), nullptr, false);
        if (!body.is_object() || body.value("identifier", string()) != cfg_.identifier ||
            body.value("password", string()) != cfg_.password) {
            return jsonResponse(http::status::unauthorized, {{"errorCode", "error.invalid.details"}});
        }
        const uint64_t salt = mix64(static_cast<uint64_t>(++logins_) * 7919 + realNowMs());
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(salt));
        const string cst = "CSTtoken" + string(buffer);
        snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(mix64(salt)));
        const string security = "SECtoken" + string(buffer);
        {
            lock_guard<mutex> lock(stateMutex_);
            validTokens_.insert(cst + "|" + security);
        }
        json out = {{"accountType", "CFD"},
                    {"accountInfo", {{"balance", cfg_.sentinelBalance}, {"deposit", 1.0}, {"available", cfg_.sentinelBalance}}},
                    {"currencyIsoCode", "USD"},
                    {"currentAccountId", "123456789"},
                    {"streamingHost", "wss://127.0.0.1:" + to_string(port_) + "/"},
                    {"accounts", json::array({{{"accountId", "123456789"}, {"accountName", "USD"}, {"preferred", true}}})},
                    {"clientId", "987654"}};
        auto res = jsonResponse(http::status::ok, out);
        res.set("CST", cst);
        res.set("X-SECURITY-TOKEN", security);
        return res;
    }

    int64_t alignShiftFor(const string& resolution) const {
        lock_guard<mutex> lock(stateMutex_);
        const auto it = alignShiftMs_.find(resolution);
        return it == alignShiftMs_.end() ? 0 : it->second;
    }

    static int64_t alignDown(const ResolutionInfo& res, int64_t ts, int64_t shift) {
        const int64_t base = res.weekly ? 4 * kDayMs : 0;  // 1970-01-05 was a Monday
        return floorDivMs(ts - base - shift, res.stepMs) * res.stepMs + base + shift;
    }

    http::response<http::string_body> handlePrices(const string& epic, const map<string, string>& query) {
        if (epic != cfg_.epic) {
            return jsonResponse(http::status::not_found, {{"errorCode", "error.prices.not-found"}});
        }
        const auto resIt = query.find("resolution");
        const ResolutionInfo* res = nullptr;
        if (resIt != query.end()) {
            for (const auto& info : resolutions()) {
                if (resIt->second == info.name) res = &info;
            }
        }
        if (res == nullptr) {
            return jsonResponse(http::status::bad_request, {{"errorCode", "error.invalid.resolution"}});
        }
        int maxBars = 10;  // the real default
        if (const auto it = query.find("max"); it != query.end()) {
            maxBars = max(1, min(1000, atoi(it->second.c_str())));
        }

        const int64_t now = serverNowMs();
        int64_t cutoff = now;  // newest bar-open allowed
        const ToMode mode = static_cast<ToMode>(toMode_.load());
        if (const auto it = query.find("to"); it != query.end() && mode != ToMode::Ignored) {
            if (mode == ToMode::Rejected) {
                return jsonResponse(http::status::bad_request, {{"errorCode", "error.invalid.to"}});
            }
            const auto parsed = candle_builder::parseUtcIso(it->second);
            if (!parsed) {
                return jsonResponse(http::status::bad_request, {{"errorCode", "error.invalid.date-format"}});
            }
            int64_t utc = *parsed;
            if (mode == ToMode::ShiftedInclusive || mode == ToMode::ShiftedExclusive) {
                utc -= static_cast<int64_t>(toShiftHours_.load()) * 3600 * 1000;
            }
            if (mode == ToMode::UtcExclusive || mode == ToMode::ShiftedExclusive) {
                utc -= 1;
            }
            cutoff = min(now, utc);
            if (mode == ToMode::IgnoresMax) {
                maxBars = min(maxBars, 10);  // `to` works, but pages stay tiny
            }
        }

        const int64_t shift = alignShiftFor(res->name);
        const int64_t floorMs = now - res->depthMs;
        vector<OBar> bars;  // newest first
        int64_t open = alignDown(*res, cutoff, shift);
        for (int64_t guard = 0; open >= floorMs && static_cast<int>(bars.size()) < maxBars &&
                                guard < res->depthMs / res->stepMs + 2;
             open -= res->stepMs, ++guard) {
            const optional<OBar> bar = truth_.restBar(open, open + res->stepMs, now);
            if (bar && bar->count > 0) {
                bars.push_back(*bar);
            }
        }
        reverse(bars.begin(), bars.end());

        json prices = json::array();
        for (const OBar& bar : bars) {
            const string utc = candle_builder::formatUtcIso(bar.openMs);
            string local = utc;
            local[4] = '/';
            local[7] = '/';
            local[10] = ' ';
            auto side = [](double v) { return json{{"bid", v}, {"ask", v + 0.0001}}; };
            prices.push_back({{"snapshotTime", local},
                              {"snapshotTimeUTC", utc},
                              {"openPrice", side(bar.o)},
                              {"closePrice", side(bar.c)},
                              {"highPrice", side(bar.h)},
                              {"lowPrice", side(bar.l)},
                              {"lastTradedVolume", bar.count}});
        }
        return jsonResponse(http::status::ok, {{"prices", prices}, {"instrumentType", "CURRENCIES"}});
    }

    Config cfg_;
    GroundTruth truth_;
    atomic<int64_t> clockShiftMs_;
    atomic<int> toMode_{0};
    atomic<int> toShiftHours_{0};
    atomic<bool> refuseSubscribe_{false};
    atomic<int> inject429_{0};

    ssl::context sslCtx_;
    net::io_context acceptIoc_;
    tcp::acceptor acceptor_;
    unsigned short port_ = 0;
    string caFile_;
    thread acceptThread_;
    atomic<bool> stopping_{false};
    mutex connsMutex_;
    vector<shared_ptr<Conn>> conns_;

    mutable mutex stateMutex_;
    unordered_set<string> validTokens_;
    map<string, int64_t> alignShiftMs_;
    vector<string> requestLog_;
    set<string> wsDestinations_;
    deque<chrono::steady_clock::time_point> requestTimes_;
    deque<string> requestLabels_;
    string peakWindow_;
    optional<chrono::steady_clock::time_point> lastLogin_;

    atomic<uint64_t> wsEpoch_{0};
    atomic<int> logins_{0};
    atomic<int> wsConnections_{0};
    atomic<int> rateViolations_{0};
    atomic<int> loginViolations_{0};
    atomic<int> unauthorized_{0};
    atomic<int> quotesSent_{0};
    atomic<int> pings_{0};
    atomic<int> maxInSecond_{0};
};

}  // namespace mockcap