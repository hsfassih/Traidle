#pragma once

#include "../utils/candle_builder.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// capital_api: everything that talks to Capital.com's REST API - credentials,
// the shared login Session, a process-wide rate limiter, a keep-alive HTTPS
// client, and the price-bar parser. The streaming WebSocket lives in
// capital.cpp; the history backfill built on top of this lives in
// capital_history.cpp.
//
// SECURITY: this API key can trade. Only market-data endpoints are ever
// called from here (/session, /time, /markets/{epic}, /prices/{epic}).
// Credentials, CST and the security token are never printed, logged or
// written to disk; error messages built in this file never contain them.
// ---------------------------------------------------------------------------
namespace capital {

using json = nlohmann::json;
namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace http = beast::http;

struct Credentials {
    string apiKey;
    string identifier;  // login e-mail
    string password;    // the API key's custom password, NOT the account password
    bool demo = false;
};

// Reads CAPITAL_API_KEY, CAPITAL_IDENTIFIER, CAPITAL_PASSWORD and the optional
// CAPITAL_ENV ("live" default, or "demo"). On failure returns nullopt and
// `error` names exactly which variables are missing/invalid (never a value).
optional<Credentials> credentialsFromEnv(string& error);

// Where the REST API lives. Production values come from endpointsFor(); the
// struct exists so the offline mock-server test can point a Session at
// 127.0.0.1 and trust its self-signed certificate (caFile). caFile is empty -
// and TLS verification is the stock verify_peer with the system CA bundle -
// everywhere else.
struct Endpoints {
    string restHost;
    string restPort = "443";
    string caFile;
};
Endpoints endpointsFor(bool demo);

// Epics are letters, digits and a few separators ("EURUSD", "US500",
// "GOLD.1", "EUR_USD"). Anything else is rejected before it can reach a URL.
bool isValidEpic(const string& epic);

// First 4 and last 3 characters of a secret, the only form in which one may
// ever be logged ("abcd...xyz"); short secrets are fully masked.
string maskSecret(const string& secret);

// Builds a TLS client context the same way binance.cpp does: TLS 1.2+, system
// CA bundle, verify_peer (plus endpoints.caFile when set).
void configureSslContext(ssl::context& ctx, const Endpoints& endpoints);

// ---------------------------------------------------------------------------
// RateLimiter: process-wide (all threads). At most kMaxRequestsPerSecond REST
// requests in any trailing second - half of Capital's 10/s per-user limit, so
// a burst of 15 workers backfilling at once can never trip a 429 - plus a
// separate gate guaranteeing >= kMinLoginIntervalMs between POST /session
// calls (Capital allows 1/s per API key; the extra 100 ms is clock-jitter margin).
// ---------------------------------------------------------------------------
class RateLimiter {
public:
    static constexpr int kMaxRequestsPerSecond = 5;
    static constexpr int kMinLoginIntervalMs = 1100;

    static RateLimiter& instance();

    // Blocks the calling thread until one more request fits in the window.
    void throttle();
    // Blocks until a login may be sent (also counts as a request).
    void throttleLogin();

private:
    mutex mutex_;
    deque<chrono::steady_clock::time_point> events_;
    optional<chrono::steady_clock::time_point> lastLogin_;
    mutex loginMutex_;
};

// ---------------------------------------------------------------------------
// HttpsConnection: one keep-alive HTTPS connection driven synchronously from
// the calling thread, with real I/O timeouts. It owns nothing but its stream;
// the io_context it runs on must be private to the thread using it (it is
// restart()ed and run() once per operation).
//
// Why async-under-the-hood: Beast's blocking calls have no timeouts, so a
// stalled server would hang a worker forever. Running each step as an async op
// on a tcp_stream with expires_after() gives every step a deadline while the
// public API stays simple and synchronous.
// ---------------------------------------------------------------------------
struct HttpResponse {
    int status = 0;
    string body;
    string cst;            // response header "CST" (login only)
    string securityToken;  // response header "X-SECURITY-TOKEN" (login only)
    optional<long> retryAfterSeconds;
    bool keepAlive = true;
};

class HttpsConnection {
public:
    HttpsConnection(net::io_context& ioc, ssl::context& ctx, string host, string port);

    // Sends one request and returns the response (any HTTP status). Reconnects
    // transparently once if a reused connection turns out to be dead. Throws
    // runtime_error on transport failure (message never contains header values).
    HttpResponse request(http::verb verb, const string& target,
                         const vector<pair<string, string>>& headers, const string& body);

    // Opens the connection now if there is none. RestClient calls this BEFORE it takes a
    // rate-limiter slot, so the (slow) DNS/TCP/TLS handshake is not spent between claiming
    // the slot and sending the request - otherwise a burst of handshakes would let requests
    // reach the server bunched up closer together than the limiter intended.
    void ensureConnected();

    void close();

private:
    void connect();
    HttpResponse requestOnce(http::verb verb, const string& target,
                             const vector<pair<string, string>>& headers, const string& body);
    template <class Initiate>
    auto runSync(Initiate&& initiate);

    net::io_context& ioc_;
    ssl::context& ctx_;
    string host_;
    string port_;
    optional<beast::ssl_stream<beast::tcp_stream>> stream_;
    beast::flat_buffer buffer_;
};

// ---------------------------------------------------------------------------
// Session: the one login every thread shares. Thread-safe.
//   - tokens (CST / X-SECURITY-TOKEN) and the streaming host are replaced
//     atomically on each login, with a monotonically increasing `generation`;
//   - refreshIfStale() is SINGLE-FLIGHT: if 15 workers all see a 401 from the
//     same expired token, exactly one performs the login (POST /session is
//     limited to 1/s) and the other 14 reuse its tokens;
//   - serverNowMs() applies the measured local-minus-server clock offset. The
//     owner's PC clock was ~3 s ahead of Capital's servers, so the raw local
//     clock must never decide candle boundaries.
// ---------------------------------------------------------------------------
struct TokenSnapshot {
    string cst;
    string securityToken;
    uint64_t generation = 0;
};

class Session {
public:
    Session(Credentials credentials, Endpoints endpoints);

    // POST /session. Blocking; throws runtime_error with a human-readable
    // explanation (never containing the key, password or tokens).
    void login();
    // Re-login unless another thread already did since `observedGeneration`.
    void refreshIfStale(uint64_t observedGeneration);

    TokenSnapshot tokens() const;
    uint64_t generation() const { return generation_.load(); }
    // e.g. "api-streaming-capital.backend-capital.com" (no scheme, may carry :port).
    string streamingHost() const;

    // Epoch ms on Capital's clock = local system clock minus the offset.
    int64_t serverNowMs() const;
    int64_t serverOffsetMs() const { return serverOffsetMs_.load(); }
    // GET /api/v1/time x3, keep the lowest-round-trip sample. Blocking. Throws
    // only if every sample failed (the previous offset is kept either way).
    void refreshClockOffset();

    const Credentials& credentials() const { return credentials_; }
    const Endpoints& endpoints() const { return endpoints_; }

private:
    void loginLocked();

    Credentials credentials_;
    Endpoints endpoints_;

    mutex loginMutex_;           // serializes logins (single-flight)
    mutable mutex tokensMutex_;  // guards the strings below
    string cst_;
    string securityToken_;
    string streamingHost_;
    atomic<uint64_t> generation_{0};
    atomic<int64_t> serverOffsetMs_{0};
};

// ---------------------------------------------------------------------------
// RestClient: authenticated GETs over a kept-alive connection. One instance per
// thread of use (each worker owns one); it has its OWN private io_context so it
// can be driven from a helper thread while the worker's main io_context keeps
// handling ticks.
//   - every request waits on the shared RateLimiter first;
//   - HTTP 401 => session.refreshIfStale(generation) and retry once;
//   - HTTP 429 / 5xx => back off (honouring Retry-After) and retry, max 6 tries.
// ---------------------------------------------------------------------------
struct HttpResult {
    int status = 0;
    json body;  // null when the response had no (valid) JSON body
};

class RestClient {
public:
    explicit RestClient(shared_ptr<Session> session);

    // Returns the final response, whatever its status (after the retry policy).
    HttpResult get(const string& target);
    // Like get() but throws runtime_error for anything other than 2xx.
    json getJson(const string& target);

    // GET /api/v1/prices/{epic}?resolution=R&max=N[&to=T]  ->  ascending bid bars.
    // `to` is passed through verbatim ("YYYY-MM-DDTHH:MM:SS"). `skipped`, if
    // given, receives how many malformed bars were dropped. `endOfHistory`, if
    // given, is set only when a `to` request returns error.prices.not-found.
    vector<candle_builder::Bar> fetchPriceBars(const string& epic, const string& resolution,
                                               int maxBars, const optional<string>& to,
                                               size_t* skipped = nullptr,
                                               bool* endOfHistory = nullptr);

    Session& session() { return *session_; }

private:
    shared_ptr<Session> session_;
    net::io_context ioc_;
    ssl::context ctx_;
    HttpsConnection connection_;
};

// Parses a /prices response: bid side of open/high/low/close, time from
// snapshotTimeUTC (parsed with our own civil-days code, never gmtime/timegm),
// volume = lastTradedVolume (a tick count; 0 if absent). Bars with non-positive
// or non-finite prices or a high/low that contradicts open/close are skipped
// and counted. The result is ascending by open time.
vector<candle_builder::Bar> parsePriceBars(const json& response, size_t* skipped = nullptr);

}  // namespace capital