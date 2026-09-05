#include "historical_get.h"

#include "candlesticks.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

namespace historical {
namespace {

constexpr auto kHost = "fapi.binance.com";
constexpr auto kPort = "443";

// Binance's REST kline endpoint accepts at most 1500 rows per request
// (confirmed against the current API docs for GET /fapi/v1/klines).
constexpr int kMaxPageLimit = 1500;

// How far back "3 years" reaches. Doesn't need to land on an exact interval
// boundary - Binance buckets whatever startTime we send into the correct
// candle boundaries on its own.
constexpr int64_t kHistoryWindowMs = int64_t(3) * 365 * 24 * 3600 * 1000;

// A candle isn't trusted as "closed" until its closeTime is this far in the
// past, to absorb clock skew / request latency between when we snapshot
// "now" and when we evaluate each row.
constexpr int64_t kClosedSafetyMarginMs = 2000;

// Binance's documented per-IP budget for USD-M futures is 2400 request
// weight / minute, shared across every request from this machine (all
// timeframe threads combined, plus the live REST polling/priming calls
// each thread also makes). We stay well under that so backfilling never
// crowds out the live path or risks a 429/418 ban.
constexpr int kSafeWeightPerMinute = 900;

int requestWeightForLimit(int limit) {
    // Weight table from GET /fapi/v1/klines docs.
    if (limit < 100) return 1;
    if (limit < 500) return 2;
    if (limit <= 1000) return 5;
    return 10;
}

// ---------------------------------------------------------------------------
// Process-wide, thread-safe rate limiter. Every timeframe thread calls
// throttle() before each historical request; it sleeps as needed so total
// weight used across ALL threads stays under kSafeWeightPerMinute within
// any trailing 60-second window.
// ---------------------------------------------------------------------------
class RateLimiter {
public:
    static RateLimiter& instance() {
        static RateLimiter limiter;
        return limiter;
    }

    void throttle(int weight) {
        unique_lock<mutex> lock(mutex_);
        for (;;) {
            prune(steady_clock::now());
            if (usedWeight_ + weight <= kSafeWeightPerMinute) {
                events_.emplace_back(steady_clock::now(), weight);
                usedWeight_ += weight;
                return;
            }
            const auto elapsed = steady_clock::now() - events_.front().first;
            const auto wait = duration_cast<milliseconds>(seconds(60) - elapsed);
            lock.unlock();
            this_thread::sleep_for(max(wait, milliseconds(200)));
            lock.lock();
        }
    }

private:
    void prune(steady_clock::time_point now) {
        while (!events_.empty() && now - events_.front().first > seconds(60)) {
            usedWeight_ -= events_.front().second;
            events_.pop_front();
        }
    }

    mutex mutex_;
    deque<pair<steady_clock::time_point, int>> events_;
    int usedWeight_ = 0;
};

// ---------------------------------------------------------------------------
// CSV helpers (deliberately independent from binance.cpp's private helpers
// of the same shape, so this module has no compile-time coupling to the
// live-streaming translation unit).
// ---------------------------------------------------------------------------
string formatUtcTimestamp(int64_t epochMs) {
    const time_t epochSeconds = static_cast<time_t>(epochMs / 1000);
    tm utcTm{};
#if defined(_WIN32)
    gmtime_s(&utcTm, &epochSeconds);
#else
    gmtime_r(&epochSeconds, &utcTm);
#endif
    ostringstream oss;
    oss << put_time(&utcTm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

optional<int64_t> parseUtcTimestampToEpochMs(const string& field) {
    tm parsed{};
    istringstream iss(field);
    iss >> get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (iss.fail()) {
        return nullopt;
    }
#if defined(_WIN32)
    const time_t seconds = _mkgmtime(&parsed);
#else
    const time_t seconds = timegm(&parsed);
#endif
    if (seconds == static_cast<time_t>(-1)) {
        return nullopt;
    }
    return static_cast<int64_t>(seconds) * 1000;
}

string formatPrice(double price) {
    ostringstream oss;
    oss << setprecision(10) << defaultfloat << price;
    return oss.str();
}

ofstream openCsvAppend(const filesystem::path& path) {
    const bool needsHeader = !filesystem::exists(path) || filesystem::file_size(path) == 0;
    ofstream csv(path, ios::app);
    if (!csv.is_open()) {
        throw runtime_error("Failed to open CSV file: " + path.string());
    }
    if (needsHeader) {
        csv << "timestamp,base_volume,quote_volume,taker_buy_base_volume,taker_buy_quote_volume,"
               "open,high,low,close\n";
        csv.flush();
    }
    return csv;
}

void writeRow(ofstream& csv, const candlesticks::Candlestick& candle) {
    csv << formatUtcTimestamp(candle.openTime) << ','
        << formatPrice(candle.baseVolume) << ','
        << formatPrice(candle.quoteVolume) << ','
        << formatPrice(candle.takerBuyBaseVolume) << ','
        << formatPrice(candle.takerBuyQuoteVolume) << ','
        << formatPrice(candle.open) << ','
        << formatPrice(candle.high) << ','
        << formatPrice(candle.low) << ','
        << formatPrice(candle.close) << '\n';
    csv.flush();
}

// Reads only the tail of the file (cheap even for a multi-million-row 1m
// CSV) and walks backwards line by line, skipping the header and any
// trailing blank/torn line, to find the last row whose timestamp parses
// cleanly. Returns that row's open time in epoch ms, or nullopt if the file
// doesn't exist / has no valid data row yet (both mean "start from scratch,
// 3 years back").
optional<int64_t> lastStoredOpenTimeMs(const filesystem::path& csvPath) {
    if (!filesystem::exists(csvPath)) {
        return nullopt;
    }
    ifstream file(csvPath, ios::binary);
    if (!file.is_open()) {
        return nullopt;
    }
    file.seekg(0, ios::end);
    const streamoff size = file.tellg();
    if (size <= 0) {
        return nullopt;
    }
    constexpr streamoff kTailBytes = 8192;
    const streamoff tail = min(size, kTailBytes);
    file.seekg(size - tail);
    string buffer(static_cast<size_t>(tail), '\0');
    file.read(buffer.data(), tail);

    // If the tail doesn't end in a newline, the final line is still being
    // written (or was torn by a crash mid-write) - drop it unconditionally
    // rather than risk trusting a half-written row.
    const bool endsWithNewline = !buffer.empty() && (buffer.back() == '\n');

    vector<string> lines;
    stringstream ss(buffer);
    string line;
    while (getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (!lines.empty() && !endsWithNewline) {
        lines.pop_back();
    }

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->rfind("timestamp,", 0) == 0 || it->empty()) {
            continue;
        }

        // Require a fully-formed row (timestamp + 4 volume fields + 4 OHLC
        // fields = 9 columns total) before trusting it as a resume point,
        // so a row corrupted or truncated by a mid-write crash is skipped
        // in favor of the last complete one rather than silently accepted
        // with garbage/partial values.
        vector<string> fields;
        stringstream fieldStream(*it);
        string field;
        while (getline(fieldStream, field, ',')) {
            fields.push_back(field);
        }
        if (fields.size() != 9) {
            continue;
        }
        bool fieldsValid = true;
        for (size_t i = 1; i < fields.size(); ++i) {
            try {
                size_t consumed = 0;
                stod(fields[i], &consumed);
                fieldsValid = fieldsValid && (consumed == fields[i].size());
            } catch (const exception&) {
                fieldsValid = false;
            }
            if (!fieldsValid) {
                break;
            }
        }
        if (!fieldsValid) {
            continue;
        }

        if (auto ms = parseUtcTimestampToEpochMs(fields[0])) {
            return ms;
        }
    }
    return nullopt;
}

// ---------------------------------------------------------------------------
// Wraps one historical page request over a kept-alive TLS connection to
// fapi.binance.com. Reconnects transparently if the server closes the
// keep-alive connection or it otherwise drops; retries on 429/418 with
// backoff honoring the Retry-After header when present.
// ---------------------------------------------------------------------------
class KlineHistoryConnection {
public:
    KlineHistoryConnection(net::io_context& ioc, ssl::context& ctx) : ioc_(ioc), ctx_(ctx) {}

    vector<candlesticks::Candlestick> fetchPage(const string& symbol, const string& interval,
                                                 int64_t startTimeMs, int64_t endTimeMs, int limit) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                ensureConnected();
                return fetchPageOnce(symbol, interval, startTimeMs, endTimeMs, limit);
            } catch (const ConnectionLost&) {
                stream_.reset();
            }
        }
        throw runtime_error("could not (re)establish historical REST connection");
    }

private:
    struct ConnectionLost : exception {};

    void ensureConnected() {
        if (stream_) {
            return;
        }
        stream_.emplace(ioc_, ctx_);
        if (!SSL_set_tlsext_host_name(stream_->native_handle(), kHost)) {
            throw beast::system_error{beast::error_code(
                static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
        }
        stream_->set_verify_callback(ssl::host_name_verification(kHost));
        tcp::resolver resolver(ioc_);
        const auto results = resolver.resolve(kHost, kPort);
        net::connect(stream_->next_layer(), results);
        stream_->handshake(ssl::stream_base::client);
        buffer_.clear();
    }

    vector<candlesticks::Candlestick> fetchPageOnce(const string& symbol, const string& interval,
                                                     int64_t startTimeMs, int64_t endTimeMs,
                                                     int limit) {
        const string target = "/fapi/v1/klines?symbol=" + symbol + "&interval=" + interval +
                               "&startTime=" + to_string(startTimeMs) +
                               "&endTime=" + to_string(endTimeMs) +
                               "&limit=" + to_string(limit);
        http::request<http::empty_body> request{http::verb::get, target, 11};
        request.set(http::field::host, kHost);
        request.set(http::field::user_agent, "Traidle");

        constexpr int kMaxRetries = 6;
        for (int retry = 0;; ++retry) {
            RateLimiter::instance().throttle(requestWeightForLimit(limit));

            beast::error_code writeError;
            http::write(*stream_, request, writeError);
            if (writeError) {
                throw ConnectionLost{};
            }

            http::response<http::string_body> response;
            beast::error_code readError;
            http::read(*stream_, buffer_, response, readError);
            if (readError == http::error::end_of_stream || readError == net::error::eof ||
                readError == net::error::connection_reset || readError == net::error::broken_pipe) {
                throw ConnectionLost{};
            }
            if (readError) {
                throw beast::system_error(readError);
            }

            if (response.result() == http::status::ok) {
                return parseKlineArray(json::parse(response.body()), symbol);
            }

            const int status = static_cast<int>(response.result_int());
            if (status == 429 || status == 418) {
                if (retry >= kMaxRetries) {
                    throw runtime_error("rate-limited/banned repeatedly (HTTP " +
                                        to_string(status) + ")");
                }
                seconds backoff{5 * (retry + 1)};
                const auto retryAfter = response.find(http::field::retry_after);
                if (retryAfter != response.end()) {
                    try {
                        backoff = seconds(stoll(string(retryAfter->value())));
                    } catch (const exception&) {
                        // Keep the default backoff if the header isn't a plain integer.
                    }
                }
                this_thread::sleep_for(backoff);
                if (!response.keep_alive()) {
                    stream_.reset();
                    ensureConnected();
                }
                continue;
            }

            throw runtime_error("historical REST request failed with HTTP " + to_string(status));
        }
    }

    // Reuses candlesticks::parseKlineResponse()'s validation (positive
    // prices, high/low consistency, volume fields, etc.) for every row by
    // wrapping each one as its own single-row response, instead of
    // duplicating that logic here. candlesticks.cpp itself is never
    // modified by this module.
    static vector<candlesticks::Candlestick> parseKlineArray(const json& body,
                                                              const string& symbol) {
        vector<candlesticks::Candlestick> candles;
        if (!body.is_array()) {
            return candles;
        }
        candles.reserve(body.size());
        for (const auto& row : body) {
            json wrapped = json::array();
            wrapped.push_back(row);
            if (auto candle = candlesticks::parseKlineResponse(wrapped, symbol)) {
                candles.push_back(*candle);
            }
        }
        return candles;
    }

    net::io_context& ioc_;
    ssl::context& ctx_;
    optional<ssl::stream<tcp::socket>> stream_;
    beast::flat_buffer buffer_;
};

int64_t nowMs() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

bool ensureContinuousHistory(net::io_context& ioc, ssl::context& ctx, const string& symbol,
                              const string& binanceInterval, const filesystem::path& csvPath,
                              const StatusHandler& status, const LogHandler& log) {
    try {
        int64_t nextNeededOpenTimeMs = nowMs() - kHistoryWindowMs;
        if (auto last = lastStoredOpenTimeMs(csvPath)) {
            nextNeededOpenTimeMs = *last + 1;
        }

        if (nextNeededOpenTimeMs >= nowMs()) {
            return true;  // already continuous, nothing to backfill
        }

        ofstream csv = openCsvAppend(csvPath);
        KlineHistoryConnection connection(ioc, ctx);

        int64_t fetchedCount = 0;
        auto lastProgressUpdate = steady_clock::now();

        // "now" is re-sampled every iteration (not captured once up front),
        // so a multi-hour backfill on a fast timeframe still converges: each
        // page can retire far more candles than close during the time the
        // request itself takes.
        for (;;) {
            const int64_t currentNowMs = nowMs();
            if (nextNeededOpenTimeMs >= currentNowMs) {
                break;
            }

            auto page = connection.fetchPage(symbol, binanceInterval, nextNeededOpenTimeMs,
                                              currentNowMs, kMaxPageLimit);
            if (page.empty()) {
                break;  // no more data in range (e.g. symbol younger than 3 years)
            }

            bool wroteAny = false;
            for (const auto& candle : page) {
                if (candle.closeTime + kClosedSafetyMarginMs > currentNowMs) {
                    // Still forming as of this check - leave it for the live
                    // path (fetchCurrentCandle / WebSocket) to pick up.
                    break;
                }
                writeRow(csv, candle);
                nextNeededOpenTimeMs = candle.openTime + 1;
                ++fetchedCount;
                wroteAny = true;
            }

            if (!wroteAny) {
                break;
            }

            // In-place status update only - this can fire hundreds of times
            // during a large 1m/3m/5m backfill, so it must never touch the
            // scrolling log (see the StatusHandler/LogHandler split above).
            if (steady_clock::now() - lastProgressUpdate > milliseconds(750)) {
                status("backfilling history: " + to_string(fetchedCount) + " candle(s) so far");
                lastProgressUpdate = steady_clock::now();
            }
        }

        if (fetchedCount > 0) {
            log("history backfill complete: added " + to_string(fetchedCount) + " candle(s)");
        }
        return true;
    } catch (const exception& e) {
        log(string("history backfill failed: ") + e.what());
        return false;
    }
}

}  // namespace historical