#include "historical_get.h"

#include "../utils/candlesticks.h"
#include "../utils/indicators.h"

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

// How far back "5 years" reaches. Doesn't need to land on an exact interval
// boundary - Binance buckets whatever startTime we send into the correct
// candle boundaries on its own.
constexpr int64_t kHistoryWindowMs = int64_t(5) * 365 * 24 * 3600 * 1000;

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

// ---------------------------------------------------------------------------
// Cancellation. `cancel` is an optional flag owned by the caller (see
// ensureContinuousHistory's comment in the header); Cancelled is thrown from
// the waits below and caught at the top of ensureContinuousHistory, so the
// rate limiter and the 429 backoff - which can sleep for a minute - never delay
// a stop request.
// ---------------------------------------------------------------------------
struct Cancelled {};

bool isCancelled(const atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load(memory_order_relaxed);
}

// Sleeps for `total`, waking every 100 ms to look at the flag; throws Cancelled
// as soon as it is set.
void sleepUnlessCancelled(steady_clock::duration total, const atomic<bool>* cancel) {
    const auto end = steady_clock::now() + total;
    for (;;) {
        if (isCancelled(cancel)) {
            throw Cancelled{};
        }
        const auto left = end - steady_clock::now();
        if (left <= steady_clock::duration::zero()) {
            return;
        }
        this_thread::sleep_for(min<steady_clock::duration>(left, milliseconds(100)));
    }
}

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

    void throttle(int weight, const atomic<bool>* cancel = nullptr) {
        unique_lock<mutex> lock(mutex_);
        for (;;) {
            if (isCancelled(cancel)) {
                throw Cancelled{};
            }
            prune(steady_clock::now());
            if (usedWeight_ + weight <= kSafeWeightPerMinute) {
                events_.emplace_back(steady_clock::now(), weight);
                usedWeight_ += weight;
                return;
            }
            const auto elapsed = steady_clock::now() - events_.front().first;
            const auto wait = duration_cast<milliseconds>(seconds(60) - elapsed);
            lock.unlock();
            sleepUnlessCancelled(max(wait, milliseconds(200)), cancel);
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
            << indicators::indicatorHeader() << "open,high,low,close\n";
        csv.flush();
    }
    return csv;
}

indicators::IndicatorSnapshot writeRow(ofstream& csv, const candlesticks::Candlestick& candle,
                                       indicators::IndicatorEngine& engine) {
    const auto snapshot = engine.update(candle);
    csv << formatUtcTimestamp(candle.openTime) << ','
        << formatPrice(candle.baseVolume) << ','
        << formatPrice(candle.quoteVolume) << ','
        << formatPrice(candle.takerBuyBaseVolume) << ','
        << formatPrice(candle.takerBuyQuoteVolume) << ','
        << indicators::formatIndicatorRow(snapshot)
        << formatPrice(candle.open) << ','
        << formatPrice(candle.high) << ','
        << formatPrice(candle.low) << ','
        << formatPrice(candle.close) << '\n';
    csv.flush();
    return snapshot;
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
                                                 int64_t startTimeMs, int64_t endTimeMs, int limit,
                                                 const atomic<bool>* cancel = nullptr) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                ensureConnected();
                return fetchPageOnce(symbol, interval, startTimeMs, endTimeMs, limit, cancel);
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
                                                     int limit, const atomic<bool>* cancel) {
        const string target = "/fapi/v1/klines?symbol=" + symbol + "&interval=" + interval +
                               "&startTime=" + to_string(startTimeMs) +
                               "&endTime=" + to_string(endTimeMs) +
                               "&limit=" + to_string(limit);
        http::request<http::empty_body> request{http::verb::get, target, 11};
        request.set(http::field::host, kHost);
        request.set(http::field::user_agent, "Traidle");

        constexpr int kMaxRetries = 6;
        for (int retry = 0;; ++retry) {
            RateLimiter::instance().throttle(requestWeightForLimit(limit), cancel);

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
                sleepUnlessCancelled(backoff, cancel);
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
                              const StatusHandler& status, const LogHandler& log,
                              indicators::IndicatorEngine& engine,
                              const HistorySeedHandler& seedHistory,
                              const atomic<bool>* cancel) {
    try {
        // Restore the engine's internal state (rolling windows, seeded
        // EMAs, VWAP's daily accumulator, OBV's running total) from
        // whatever is already on disk BEFORE computing or writing a
        // single new indicator value below. Unconditional and always
        // first - even in the "already continuous, nothing to backfill"
        // early-return case just below, the engine still needs to be
        // caught up before it's handed to the live path.
        csv_history::replayCsvIntoEngine(csvPath, engine, seedHistory, cancel);
        if (isCancelled(cancel)) {
            return false;
        }

        int64_t nextNeededOpenTimeMs = nowMs() - kHistoryWindowMs;
        if (auto last = csv_history::lastStoredCsvOpenTimeMs(csvPath)) {
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

            if (isCancelled(cancel)) {
                return false;
            }
            auto page = connection.fetchPage(symbol, binanceInterval, nextNeededOpenTimeMs,
                                              currentNowMs, kMaxPageLimit, cancel);
            if (page.empty()) {
                break;  // no more data in range (e.g. symbol younger than 3 years)
            }

            bool wroteAny = false;
            for (const auto& candle : page) {
                if (isCancelled(cancel)) {
                    return false;  // every row written so far is complete; the next run resumes here
                }
                if (candle.closeTime + kClosedSafetyMarginMs > currentNowMs) {
                    // Still forming as of this check - leave it for the live
                    // path (fetchCurrentCandle / WebSocket) to pick up.
                    break;
                }
                const auto snapshot = writeRow(csv, candle, engine);
                if (seedHistory) {
                    // parseKlineResponse() always reports closed=false (a
                    // REST response has no direct way to signal this) -
                    // but the closeTime check just above already proves
                    // this candle is safely in the past, so we know
                    // definitively it's closed. Only the copy handed to
                    // seedHistory needs the corrected flag; the CSV itself
                    // doesn't store this field at all, so `candle` is left
                    // untouched for everything else in this loop.
                    candlesticks::Candlestick closedCandle = candle;
                    closedCandle.closed = true;
                    seedHistory(closedCandle, snapshot);
                }
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
    } catch (const Cancelled&) {
        return false;  // stop requested during a rate-limit / 429 wait: not an error, nothing to report
    } catch (const exception& e) {
        log(string("history backfill failed: ") + e.what());
        return false;
    }
}

}  // namespace historical