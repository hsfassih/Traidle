#include "capital_api.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/use_future.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <future>
#include <stdexcept>
#include <thread>

using tcp = boost::asio::ip::tcp;
using namespace std;
using namespace std::chrono;

namespace capital {

namespace {

// How long each connection step may take before it is treated as dead. Same 15 s
// the Binance sessions use for resolve/connect/handshake.
constexpr auto kConnectTimeout = seconds(15);
// Per read/write on an established connection. Generous because a 1000-bar
// /prices response is a few hundred KB.
constexpr auto kIoTimeout = seconds(30);
// Upper bound on one REST response body. A 1000-bar price page is ~350 KB.
constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;

// REST retry policy for HTTP 429 / 5xx: up to this many tries in total, with
// exponential backoff (1, 2, 4, 8, 16 s) unless the server sends Retry-After.
constexpr int kMaxTries = 6;
constexpr long kMaxBackoffSeconds = 60;

string trimCopy(const string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

string envValue(const char* name) {
    const char* raw = getenv(name);
    return raw == nullptr ? string() : trimCopy(raw);
}

int64_t localNowMs() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// The server's errorCode is a short token like "error.invalid.api.key"; keep it
// only if it looks like one, so an unexpected body can never inject arbitrary
// text into a console line.
string safeErrorCode(const string& body) {
    const json parsed = json::parse(body, nullptr, false);
    if (!parsed.is_object()) {
        return {};
    }
    const auto it = parsed.find("errorCode");
    if (it == parsed.end() || !it->is_string()) {
        return {};
    }
    const string code = it->get<string>();
    if (code.empty() || code.size() > 64) {
        return {};
    }
    for (const char c : code) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return {};
        }
    }
    return code;
}

string getErrorMessage(const string& target, const HttpResult& result) {
    const string path = target.substr(0, target.find('?'));
    string message = "Capital.com GET " + path + " failed with HTTP " + to_string(result.status);
    if (result.body.is_object()) {
        const string code = safeErrorCode(result.body.dump());
        if (!code.empty()) {
            message += " (" + code + ")";
        }
    }
    return message;
}

// Turns a failed POST /session into something a person can act on. Never
// includes the API key, e-mail, password or any token (the key appears only in
// its masked form, which is allowed).
string explainLoginFailure(int status, const string& body, const Credentials& credentials) {
    const string code = safeErrorCode(body);
    const string environment = credentials.demo ? "demo" : "live";
    if (status == 401 && code == "error.invalid.api.key") {
        return "Capital.com rejected the API key (error.invalid.api.key; key " +
               maskSecret(credentials.apiKey) + "). Check CAPITAL_API_KEY, and that the key was created "
               "for the same environment as CAPITAL_ENV (now '" + environment +
               "'): a demo key does not work on live and vice versa.";
    }
    if (status == 401 && code == "error.invalid.details") {
        return "Capital.com rejected the login (error.invalid.details). Check CAPITAL_IDENTIFIER (your "
               "login e-mail) and that CAPITAL_PASSWORD is the custom password you set when you created "
               "the API key, not your Capital.com account password.";
    }
    if (status == 429) {
        return "Capital.com is rate limiting logins (HTTP 429, error.too-many.requests). Wait a few "
               "seconds and start again.";
    }
    string message = "Capital.com login failed with HTTP " + to_string(status);
    if (!code.empty()) {
        message += " (" + code + ")";
    }
    return message;
}

// "wss://api-streaming-capital.backend-capital.com/" -> the bare host (a ":port"
// suffix is kept; production has none). Falls back to the documented host for
// the environment if the login response did not carry one.
string normalizeStreamingHost(string raw, bool demo) {
    for (const char* scheme : {"wss://", "ws://", "https://", "http://"}) {
        const string prefix = scheme;
        if (raw.rfind(prefix, 0) == 0) {
            raw.erase(0, prefix.size());
            break;
        }
    }
    const auto slash = raw.find('/');
    if (slash != string::npos) {
        raw.erase(slash);
    }
    raw = trimCopy(raw);
    if (raw.empty()) {
        return demo ? "demo-api-streaming-capital.backend-capital.com"
                    : "api-streaming-capital.backend-capital.com";
    }
    return raw;
}

}  // namespace

// ---------------------------------------------------------------------------
// Credentials / endpoints / small helpers
// ---------------------------------------------------------------------------
optional<Credentials> credentialsFromEnv(string& error) {
    Credentials credentials;
    credentials.apiKey = envValue("CAPITAL_API_KEY");
    credentials.identifier = envValue("CAPITAL_IDENTIFIER");
    credentials.password = envValue("CAPITAL_PASSWORD");

    vector<string> missing;
    if (credentials.apiKey.empty()) missing.push_back("CAPITAL_API_KEY");
    if (credentials.identifier.empty()) missing.push_back("CAPITAL_IDENTIFIER");
    if (credentials.password.empty()) missing.push_back("CAPITAL_PASSWORD");
    if (!missing.empty()) {
        error = "Missing environment variable(s): ";
        for (size_t i = 0; i < missing.size(); ++i) {
            error += (i == 0 ? "" : ", ") + missing[i];
        }
        error += ". Set them in the environment or in a .env file in the folder you run Traidle from "
                 "(see README).";
        return nullopt;
    }

    string environment = envValue("CAPITAL_ENV");
    transform(environment.begin(), environment.end(), environment.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (environment.empty() || environment == "live") {
        credentials.demo = false;
    } else if (environment == "demo") {
        credentials.demo = true;
    } else {
        error = "CAPITAL_ENV must be 'live' or 'demo' (got '" + environment + "').";
        return nullopt;
    }
    return credentials;
}

Endpoints endpointsFor(bool demo) {
    Endpoints endpoints;
    endpoints.restHost = demo ? "demo-api-capital.backend-capital.com" : "api-capital.backend-capital.com";
    endpoints.restPort = "443";
    return endpoints;
}

bool isValidEpic(const string& epic) {
    if (epic.empty() || epic.size() > 40) {
        return false;
    }
    for (const char c : epic) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

string maskSecret(const string& secret) {
    // Showing 7 characters of a short secret would reveal most of it.
    if (secret.size() < 12) {
        return "***";
    }
    return secret.substr(0, 4) + "..." + secret.substr(secret.size() - 3);
}

void configureSslContext(ssl::context& ctx, const Endpoints& endpoints) {
    // ctx.load_verify_file("C:/certs/cacert.pem"); // for locally running on windows without docker
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);
    if (!endpoints.caFile.empty()) {
        ctx.load_verify_file(endpoints.caFile);
    }
}

// ---------------------------------------------------------------------------
// RateLimiter
// ---------------------------------------------------------------------------
RateLimiter& RateLimiter::instance() {
    static RateLimiter limiter;
    return limiter;
}

void RateLimiter::throttle() {
    unique_lock<mutex> lock(mutex_);
    for (;;) {
        const auto now = steady_clock::now();
        while (!events_.empty() && now - events_.front() >= seconds(1)) {
            events_.pop_front();
        }
        if (static_cast<int>(events_.size()) < kMaxRequestsPerSecond) {
            events_.push_back(now);
            return;
        }
        const auto wait = duration_cast<milliseconds>(seconds(1) - (now - events_.front())) + milliseconds(5);
        lock.unlock();
        this_thread::sleep_for(max(wait, milliseconds(5)));
        lock.lock();
    }
}

void RateLimiter::throttleLogin() {
    // Held for the whole wait so concurrent logins queue up one behind another.
    lock_guard<mutex> gate(loginMutex_);
    if (lastLogin_) {
        const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - *lastLogin_);
        const auto minimum = milliseconds(kMinLoginIntervalMs);
        if (elapsed < minimum) {
            this_thread::sleep_for(minimum - elapsed);
        }
    }
    throttle();
    lastLogin_ = steady_clock::now();
}

// ---------------------------------------------------------------------------
// HttpsConnection
// ---------------------------------------------------------------------------
HttpsConnection::HttpsConnection(net::io_context& ioc, ssl::context& ctx, string host, string port)
    : ioc_(ioc), ctx_(ctx), host_(move(host)), port_(move(port)) {}

void HttpsConnection::close() { stream_.reset(); }

template <class Initiate>
auto HttpsConnection::runSync(Initiate&& initiate) {
    // Start one async operation, run the private io_context until it (and its
    // timeout bookkeeping) is done, then fetch the result - rethrowing any
    // error, including Beast's timeout error.
    ioc_.restart();
    auto future = initiate(net::use_future);
    ioc_.run();
    return future.get();
}

void HttpsConnection::connect() {
    stream_.emplace(ioc_, ctx_);
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host_.c_str())) {
        throw beast::system_error{beast::error_code(
            static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
    }
    stream_->set_verify_callback(ssl::host_name_verification(host_));

    tcp::resolver resolver(ioc_);
    const auto results = runSync([&](auto&& token) {
        return resolver.async_resolve(host_, port_, std::forward<decltype(token)>(token));
    });

    beast::get_lowest_layer(*stream_).expires_after(kConnectTimeout);
    runSync([&](auto&& token) {
        return beast::get_lowest_layer(*stream_).async_connect(results,
                                                               std::forward<decltype(token)>(token));
    });

    beast::get_lowest_layer(*stream_).expires_after(kConnectTimeout);
    runSync([&](auto&& token) {
        return stream_->async_handshake(ssl::stream_base::client, std::forward<decltype(token)>(token));
    });

    beast::get_lowest_layer(*stream_).expires_never();
    buffer_.clear();
}

HttpResponse HttpsConnection::requestOnce(http::verb verb, const string& target,
                                          const vector<pair<string, string>>& headers,
                                          const string& body) {
    http::request<http::string_body> request{verb, target, 11};
    request.set(http::field::host, host_);
    request.set(http::field::user_agent, "Traidle");
    for (const auto& header : headers) {
        request.set(header.first, header.second);
    }
    if (!body.empty()) {
        request.body() = body;
        request.prepare_payload();
    }

    beast::get_lowest_layer(*stream_).expires_after(kIoTimeout);
    runSync([&](auto&& token) {
        return http::async_write(*stream_, request, std::forward<decltype(token)>(token));
    });

    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaxBodyBytes);
    beast::get_lowest_layer(*stream_).expires_after(kIoTimeout);
    runSync([&](auto&& token) {
        return http::async_read(*stream_, buffer_, parser, std::forward<decltype(token)>(token));
    });
    beast::get_lowest_layer(*stream_).expires_never();

    auto& response = parser.get();
    HttpResponse result;
    result.status = static_cast<int>(response.result_int());
    result.keepAlive = response.keep_alive();
    if (const auto it = response.find("CST"); it != response.end()) {
        result.cst = string(it->value());
    }
    if (const auto it = response.find("X-SECURITY-TOKEN"); it != response.end()) {
        result.securityToken = string(it->value());
    }
    if (const auto it = response.find(http::field::retry_after); it != response.end()) {
        try {
            result.retryAfterSeconds = stol(string(it->value()));
        } catch (const exception&) {
            // Not a plain integer (an HTTP date, say): ignore and use our own backoff.
        }
    }
    result.body = move(response.body());
    return result;
}

void HttpsConnection::ensureConnected() {
    if (stream_) {
        return;
    }
    try {
        connect();
    } catch (const exception& e) {
        stream_.reset();
        throw runtime_error(string("HTTPS request to ") + host_ + " failed: " + e.what());
    }
}

HttpResponse HttpsConnection::request(http::verb verb, const string& target,
                                      const vector<pair<string, string>>& headers,
                                      const string& body) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool reused = stream_.has_value();
        try {
            if (!stream_) {
                connect();
            }
            HttpResponse response = requestOnce(verb, target, headers, body);
            if (!response.keepAlive) {
                stream_.reset();
            }
            return response;
        } catch (const exception& e) {
            stream_.reset();
            // A kept-alive connection the server has since closed fails on first
            // use; that is routine, so retry once on a fresh connection. A failure
            // on a connection we just opened is real.
            if (!reused || attempt == 1) {
                throw runtime_error(string("HTTPS request to ") + host_ + " failed: " + e.what());
            }
        }
    }
    throw runtime_error("HTTPS request failed");  // not reachable; keeps compilers quiet
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
Session::Session(Credentials credentials, Endpoints endpoints)
    : credentials_(move(credentials)), endpoints_(move(endpoints)) {}

void Session::login() {
    lock_guard<mutex> lock(loginMutex_);
    loginLocked();
}

void Session::refreshIfStale(uint64_t observedGeneration) {
    lock_guard<mutex> lock(loginMutex_);
    if (generation_.load() != observedGeneration) {
        return;  // another thread already re-logged in while we waited for the lock
    }
    loginLocked();
}

void Session::loginLocked() {
    RateLimiter::instance().throttleLogin();

    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    configureSslContext(ctx, endpoints_);
    HttpsConnection connection(ioc, ctx, endpoints_.restHost, endpoints_.restPort);

    json requestBody = {{"identifier", credentials_.identifier},
                        {"password", credentials_.password},
                        {"encryptedPassword", false}};
    const HttpResponse response = connection.request(
        http::verb::post, "/api/v1/session",
        {{"X-CAP-API-KEY", credentials_.apiKey}, {"Content-Type", "application/json"}},
        requestBody.dump());

    if (response.status != 200) {
        throw runtime_error(explainLoginFailure(response.status, response.body, credentials_));
    }
    if (response.cst.empty() || response.securityToken.empty()) {
        throw runtime_error("Capital.com accepted the login but returned no session tokens.");
    }

    string host;
    const json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_object()) {
        const auto it = parsed.find("streamingHost");
        if (it != parsed.end() && it->is_string()) {
            host = it->get<string>();
        }
    }
    // Deliberately keep nothing else from the body: it also lists accounts and
    // balances, which this program must never hold on to or print.

    lock_guard<mutex> tokens(tokensMutex_);
    cst_ = response.cst;
    securityToken_ = response.securityToken;
    streamingHost_ = normalizeStreamingHost(host, credentials_.demo);
    generation_.fetch_add(1);
}

TokenSnapshot Session::tokens() const {
    lock_guard<mutex> lock(tokensMutex_);
    TokenSnapshot snapshot;
    snapshot.cst = cst_;
    snapshot.securityToken = securityToken_;
    snapshot.generation = generation_.load();
    return snapshot;
}

string Session::streamingHost() const {
    lock_guard<mutex> lock(tokensMutex_);
    return streamingHost_;
}

int64_t Session::serverNowMs() const { return localNowMs() - serverOffsetMs_.load(); }

void Session::refreshClockOffset() {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    configureSslContext(ctx, endpoints_);
    HttpsConnection connection(ioc, ctx, endpoints_.restHost, endpoints_.restPort);

    // Three samples; the one with the smallest round trip has the least
    // asymmetric-latency error, so its midpoint estimate is the best.
    optional<int64_t> bestOffset;
    int64_t bestRoundTripMs = INT64_MAX;
    string lastError = "no response";
    for (int sample = 0; sample < 3; ++sample) {
        try {
            RateLimiter::instance().throttle();
            const auto steadyStart = steady_clock::now();
            const int64_t localBefore = localNowMs();
            const HttpResponse response =
                connection.request(http::verb::get, "/api/v1/time", {}, "");
            const int64_t localAfter = localNowMs();
            const int64_t roundTripMs =
                duration_cast<milliseconds>(steady_clock::now() - steadyStart).count();
            if (response.status != 200) {
                lastError = "HTTP " + to_string(response.status);
                continue;
            }
            const json parsed = json::parse(response.body, nullptr, false);
            if (!parsed.is_object() || !parsed.contains("serverTime") ||
                !parsed["serverTime"].is_number()) {
                lastError = "unexpected /time response";
                continue;
            }
            const int64_t serverTime = static_cast<int64_t>(llround(parsed["serverTime"].get<double>()));
            const int64_t offset = (localBefore + localAfter) / 2 - serverTime;
            if (roundTripMs < bestRoundTripMs) {
                bestRoundTripMs = roundTripMs;
                bestOffset = offset;
            }
        } catch (const exception& e) {
            lastError = e.what();
        }
    }
    if (!bestOffset) {
        throw runtime_error("could not read Capital.com server time: " + lastError);
    }
    serverOffsetMs_.store(*bestOffset);
}

// ---------------------------------------------------------------------------
// RestClient
// ---------------------------------------------------------------------------
RestClient::RestClient(shared_ptr<Session> session)
    : session_(move(session)),
      ctx_(ssl::context::tlsv12_client),
      connection_(ioc_, ctx_, session_->endpoints().restHost, session_->endpoints().restPort) {
    configureSslContext(ctx_, session_->endpoints());
}

HttpResult RestClient::get(const string& target) {
    bool refreshed = false;
    for (int attempt = 1;; ++attempt) {
        connection_.ensureConnected();  // handshake first, then claim the rate slot
        RateLimiter::instance().throttle();
        const TokenSnapshot tokens = session_->tokens();
        const HttpResponse response = connection_.request(
            http::verb::get, target,
            {{"CST", tokens.cst}, {"X-SECURITY-TOKEN", tokens.securityToken}}, "");

        if (response.status >= 200 && response.status < 300) {
            HttpResult result;
            result.status = response.status;
            result.body = json::parse(response.body, nullptr, false);
            if (result.body.is_discarded()) {
                result.body = json();
            }
            return result;
        }

        // Tokens expire 10 minutes after last use; the next request then fails
        // with 401. Re-login (single-flight across threads) and retry once.
        if (response.status == 401 && !refreshed) {
            refreshed = true;
            session_->refreshIfStale(tokens.generation);
            continue;
        }

        if ((response.status == 429 || response.status >= 500) && attempt < kMaxTries) {
            long backoff = min<long>(1L << (attempt - 1), 30);
            if (response.retryAfterSeconds) {
                backoff = min(max(*response.retryAfterSeconds, 1L), kMaxBackoffSeconds);
            }
            this_thread::sleep_for(seconds(backoff));
            continue;
        }

        HttpResult result;
        result.status = response.status;
        result.body = json::parse(response.body, nullptr, false);
        if (result.body.is_discarded()) {
            result.body = json();
        }
        return result;
    }
}

json RestClient::getJson(const string& target) {
    HttpResult result = get(target);
    if (result.status >= 200 && result.status < 300) {
        return move(result.body);
    }
    throw runtime_error(getErrorMessage(target, result));
}

vector<candle_builder::Bar> RestClient::fetchPriceBars(const string& epic, const string& resolution,
                                                       int maxBars, const optional<string>& to,
                                                       size_t* skipped, bool* endOfHistory) {
    if (endOfHistory != nullptr) {
        *endOfHistory = false;
    }
    if (!isValidEpic(epic)) {
        throw runtime_error("invalid epic '" + epic + "'");
    }
    string target = "/api/v1/prices/" + epic + "?resolution=" + resolution +
                    "&max=" + to_string(maxBars);
    if (to) {
        target += "&to=" + *to;
    }
    const HttpResult result = get(target);
    if (result.status >= 200 && result.status < 300) {
        return parsePriceBars(result.body, skipped);
    }
    if (to && result.status == 404 &&
        safeErrorCode(result.body.dump()) == "error.prices.not-found") {
        if (skipped != nullptr) {
            *skipped = 0;
        }
        if (endOfHistory != nullptr) {
            *endOfHistory = true;
        }
        return {};
    }
    throw runtime_error(getErrorMessage(target, result));
}

// ---------------------------------------------------------------------------
// Price-bar parsing
// ---------------------------------------------------------------------------
namespace {

optional<double> bidOf(const json& row, const char* key) {
    const auto side = row.find(key);
    if (side == row.end() || !side->is_object()) {
        return nullopt;
    }
    const auto bid = side->find("bid");
    if (bid == side->end() || !bid->is_number()) {
        return nullopt;
    }
    const double value = bid->get<double>();
    if (!isfinite(value) || value <= 0.0) {
        return nullopt;
    }
    return value;
}

optional<candle_builder::Bar> parseOneBar(const json& row) {
    if (!row.is_object()) {
        return nullopt;
    }
    const auto time = row.find("snapshotTimeUTC");
    if (time == row.end() || !time->is_string()) {
        return nullopt;
    }
    const auto openMs = candle_builder::parseUtcIso(time->get<string>());
    const auto open = bidOf(row, "openPrice");
    const auto high = bidOf(row, "highPrice");
    const auto low = bidOf(row, "lowPrice");
    const auto close = bidOf(row, "closePrice");
    if (!openMs || !open || !high || !low || !close) {
        return nullopt;
    }
    if (*high < max(*open, *close) || *low > min(*open, *close)) {
        return nullopt;  // internally inconsistent bar
    }
    candle_builder::Bar bar;
    bar.openMs = *openMs;
    bar.o = *open;
    bar.h = *high;
    bar.l = *low;
    bar.c = *close;
    // lastTradedVolume is a TICK COUNT, absent on some very old weekly bars.
    const auto volume = row.find("lastTradedVolume");
    bar.volume = (volume != row.end() && volume->is_number()) ? max(0.0, volume->get<double>()) : 0.0;
    return bar;
}

}  // namespace

vector<candle_builder::Bar> parsePriceBars(const json& response, size_t* skipped) {
    vector<candle_builder::Bar> bars;
    size_t bad = 0;
    if (response.is_object()) {
        const auto prices = response.find("prices");
        if (prices != response.end() && prices->is_array()) {
            bars.reserve(prices->size());
            for (const auto& row : *prices) {
                if (auto bar = parseOneBar(row)) {
                    bars.push_back(*bar);
                } else {
                    ++bad;
                }
            }
        }
    }
    // The API returns ascending bars (verified); this only guards the invariant
    // everything downstream relies on.
    if (!is_sorted(bars.begin(), bars.end(), [](const candle_builder::Bar& a, const candle_builder::Bar& b) {
            return a.openMs < b.openMs;
        })) {
        stable_sort(bars.begin(), bars.end(), [](const candle_builder::Bar& a, const candle_builder::Bar& b) {
            return a.openMs < b.openMs;
        });
    }
    if (skipped != nullptr) {
        *skipped = bad;
    }
    return bars;
}

}  // namespace capital