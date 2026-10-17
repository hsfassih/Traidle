#include "binance.h"
#include "candlesticks.h"
#include "historical_get.h"
#include "indicators.h"
#include "visualization.h"
#include "smc-ict/smc_engine.h"
#include "pipeline_common.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using namespace std;

// The console/CSV/visualizer glue that used to be defined right here in an
// anonymous namespace (TerminalBoard, TimeframeTask, formatPrice,
// writeClosedCandleRow, ...) now lives in pipeline_common.{h,cpp} so the
// Capital.com feed can share it. Pulling the namespace in keeps every line of
// runTimeframeStream() below byte-for-byte identical to what it was.
using namespace pipeline;

namespace {

constexpr auto kHost = "fapi.binance.com";
constexpr auto kPort = "443";
constexpr auto kWsHost = "fstream.binance.com";
constexpr auto kWsPort = "443";

// ---------------------------------------------------------------------------
// REST fetch (byte-for-byte the original, validated implementation)
// ---------------------------------------------------------------------------
optional<candlesticks::Candlestick> fetchCurrentCandle(
    net::io_context& ioc, ssl::context& ctx, const string& symbol, const string& interval) {
    tcp::resolver resolver(ioc);
    ssl::stream<tcp::socket> stream(ioc, ctx);

    if (!SSL_set_tlsext_host_name(stream.native_handle(), kHost)) {
        throw beast::system_error{beast::error_code(
            static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
    }
    stream.set_verify_callback(ssl::host_name_verification(kHost));

    const auto results = resolver.resolve(kHost, kPort);
    net::connect(stream.next_layer(), results);
    stream.handshake(ssl::stream_base::client);

    const string target = "/fapi/v1/klines?symbol=" + symbol + "&interval=" + interval +
                          "&limit=1";
    http::request<http::empty_body> request{http::verb::get, target, 11};
    request.set(http::field::host, kHost);
    request.set(http::field::user_agent, "Traidle");
    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);

    beast::error_code shutdownError;
    stream.shutdown(shutdownError);
    if (shutdownError == net::error::eof || shutdownError == ssl::error::stream_truncated) {
        shutdownError = {};
    }
    if (shutdownError) {
        throw beast::system_error(shutdownError);
    }
    if (response.result() != http::status::ok) {
        throw runtime_error("Binance REST request failed with HTTP " +
                            to_string(response.result_int()));
    }

    return candlesticks::parseKlineResponse(json::parse(response.body()), symbol);
}

// ---------------------------------------------------------------------------
// KlineStreamSession: identical resolve/connect/handshake/read/reconnect
// behavior to the validated Phase B implementation. The ONLY change is that
// its two internal diagnostic lines (connect confirmation, parse-error
// notice) now go through an `onLog` callback instead of writing to `cerr`
// directly, so a background stream thread can never write to the console
// outside the synchronized TerminalBoard below (which would otherwise
// corrupt the multi-line in-place display).
// ---------------------------------------------------------------------------
class KlineStreamSession : public std::enable_shared_from_this<KlineStreamSession> {
public:
    using CandleHandler = function<void(const candlesticks::Candlestick&)>;
    using ConnectedHandler = function<void()>;
    using ErrorHandler = function<void(const char* /*step*/, beast::error_code)>;
    using LogHandler = function<void(const string&)>;

    KlineStreamSession(net::io_context& ioc, ssl::context& ctx, string target,
                        CandleHandler onCandle, ConnectedHandler onConnected,
                        ErrorHandler onError, LogHandler onLog)
        : resolver_(net::make_strand(ioc)),
          ws_(net::make_strand(ioc), ctx),
          target_(move(target)),
          onCandle_(move(onCandle)),
          onConnected_(move(onConnected)),
          onError_(move(onError)),
          onLog_(move(onLog)) {
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), kWsHost)) {
            throw beast::system_error{beast::error_code(
                static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
        }
        ws_.next_layer().set_verify_callback(ssl::host_name_verification(kWsHost));
    }

    void run() {
        resolver_.async_resolve(kWsHost, kWsPort,
            beast::bind_front_handler(&KlineStreamSession::onResolve, shared_from_this()));
    }

private:
    void onResolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) return fail(ec, "resolve");
        beast::get_lowest_layer(ws_).expires_after(chrono::seconds(15));
        beast::get_lowest_layer(ws_).async_connect(results,
            beast::bind_front_handler(&KlineStreamSession::onConnect, shared_from_this()));
    }

    void onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) return fail(ec, "tcp_connect");
        beast::get_lowest_layer(ws_).expires_after(chrono::seconds(15));
        ws_.next_layer().async_handshake(ssl::stream_base::client,
            beast::bind_front_handler(&KlineStreamSession::onSslHandshake, shared_from_this()));
    }

    void onSslHandshake(beast::error_code ec) {
        if (ec) return fail(ec, "tls_handshake");
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "Traidle");
        }));
        ws_.async_handshake(kWsHost, target_,
            beast::bind_front_handler(&KlineStreamSession::onWsHandshake, shared_from_this()));
    }

    void onWsHandshake(beast::error_code ec) {
        if (ec) return fail(ec, "ws_upgrade");
        onLog_("connected: " + target_);
        onConnected_();
        doRead();
    }

    void doRead() {
        ws_.async_read(buffer_,
            beast::bind_front_handler(&KlineStreamSession::onRead, shared_from_this()));
    }

    void onRead(beast::error_code ec, size_t) {
        if (ec) return fail(ec, "read");
        try {
            const auto message = json::parse(beast::buffers_to_string(buffer_.data()));
            buffer_.consume(buffer_.size());
            if (auto candle = candlesticks::parseKlineMessage(message)) {
                onCandle_(*candle);
            }
        } catch (const exception& e) {
            onLog_(string("parse error: ") + e.what());
            buffer_.consume(buffer_.size());
        }
        doRead();
    }

    void fail(beast::error_code ec, const char* what) { onError_(what, ec); }

    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    string target_;
    CandleHandler onCandle_;
    ConnectedHandler onConnected_;
    ErrorHandler onError_;
    LogHandler onLog_;
};

}  // namespace

namespace binance {

// ---------------------------------------------------------------------------
// One fully isolated worker per timeframe: own io_context/ssl::context, own
// CSV file, own REST-priming/WS-reconnect/watchdog/REST-fallback state, and
// its own IndicatorEngine and SmcEngine. A failure or fallback on one
// timeframe cannot affect any other.
// ---------------------------------------------------------------------------
void runTimeframeStream(const string& symbol, TimeframeTask task, filesystem::path csvPath,
                        TerminalBoard& board, visualization::VisualizationServer& vizServer) {
    const string label = task.rawTimeframe;
    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
         // ctx.load_verify_file("C:/certs/cacert.pem"); // for locally running on windows without docker
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        // One IndicatorEngine for this timeframe's entire lifetime - handed
        // to ensureContinuousHistory() below (which warms it up from
        // whatever is already on disk and keeps updating it through
        // backfill), then reused unmodified by the live path further down.
        // Constructing a second, fresh engine for the live path would
        // silently reset every indicator's warm-up right at the seam
        // between "history that existed before this run" and "candles
        // this run has actually seen".
        indicators::IndicatorEngine engine;

        // Same one-per-timeframe-lifetime treatment as `engine` above, for
        // exactly the same reason - except nothing here is ever written to
        // the CSV (see smc-ict/smc_engine.h). historical_get.cpp has no
        // knowledge of this and is not touched by any of this: the
        // seedHistory callback already threaded through
        // ensureContinuousHistory() below (originally added only for the
        // visualization ring buffer) is reused as the replay hook instead
        // - see the accumulation into smcReplayCandles/smcReplayAtrs just
        // below and the replayHistory() call once backfill completes.
        smc::SmcEngine smcEngine;
        vector<candlesticks::Candlestick> smcReplayCandles;
        vector<optional<double>> smcReplayAtrs;

        // Before touching the live path at all: make sure the CSV is a
        // continuous history from 3 years ago up to the candle that's
        // currently forming. Covers a brand-new symbol (nothing on disk
        // yet), a resumed/interrupted prior backfill, and a plain restart
        // gap - all with the same logic. historical_get.cpp manages its own
        // CSV handle for this and hands back a file that's ready for the
        // live path to open in append mode below.
        board.updateLine(task.rowIndex, "checking historical data...");
        const bool historyOk = historical::ensureContinuousHistory(
            ioc, ctx, symbol, task.binanceInterval, csvPath,
            [&](const string& msg) { board.updateLine(task.rowIndex, msg); },
            [&](const string& msg) { board.log("[" + label + "] " + msg); },
            engine,
            [&](const candlesticks::Candlestick& candle, const indicators::IndicatorSnapshot& snapshot) {
                // Seeds the visualizer's per-timeframe history ring buffer
                // from every row already on disk (replayed) plus every
                // freshly backfilled row - never broadcast live, just
                // available so a browser connecting right after startup
                // has recent context to draw immediately.
                vizServer.seedHistory(buildVisualizationMessage(symbol, label, candle, snapshot));
                // Same candle, kept for the SmcEngine replay right after
                // this callback finishes (see below) - this is the only
                // place a full-history candle list is assembled, so
                // smc_engine.cpp's replay sees exactly the same OHLCV rows
                // IndicatorEngine's own replay just saw.
                smcReplayCandles.push_back(candle);
                smcReplayAtrs.push_back(snapshot.atr);
            });
        if (!historyOk) {
            board.log("[" + label +
                      "] historical backfill did not fully complete; continuing with live data "
                      "only (some earlier candles may still be missing).");
        }

        // SMC/ICT replay: one ordinary sequential pass over the same
        // candles just reconstructed above - see smc_engine.h's class
        // comment for why this is not chunked across threads. Then push
        // the resulting "currently active structures" snapshot once, so a
        // browser that connects before the first live candle closes still
        // sees the full picture immediately rather than an empty one.
        board.updateLine(task.rowIndex, "computing SMC/ICT structures...");
        smcEngine.replayHistory(smcReplayCandles, smcReplayAtrs);
        smcReplayCandles.clear();
        smcReplayCandles.shrink_to_fit();
        smcReplayAtrs.clear();
        smcReplayAtrs.shrink_to_fit();
        vizServer.setSmcSnapshot(symbol, label, smcEngine.currentSnapshot());

        ofstream csv = openCsvAppend(csvPath);

        auto renderLine = [&](const candlesticks::Candlestick& candle, const char* tag) {
            const size_t columns = terminalColumns();
            string line = formatCandleLine(symbol, candle, tag, false);
            if (columns > 0 && (label.size() + 3 + line.size()) >= columns) {
                line = formatCandleLine(symbol, candle, tag, true);
            }
            board.updateLine(task.rowIndex, line);

            // Live/forming-candle tap point for the visualization layer.
            // Uses engine.peek() - a read-only "what if this candle
            // closed right now" preview - rather than engine.update(),
            // so RSI/MACD/Stoch/ATR/VWAP/OBV in the UI track the current,
            // still-moving price in real time instead of freezing at
            // whatever they were as of the last CLOSED candle (which
            // could be up to a full timeframe period stale). peek()
            // never mutates `engine` itself - see indicators.h - so the
            // original invariant ("this candle must not permanently
            // affect the engine's rolling state until it actually
            // closes") is fully preserved. As a useful side effect, this
            // also keeps the WebSocket link under roughly constant
            // traffic during active trading (about once per second per
            // timeframe), rather than only at candle-close boundaries.
            vizServer.push(buildVisualizationMessage(symbol, label, candle, engine.peek(candle)));
        };

        // Tracks the last candle seen from ANY source so we can detect a
        // rollover even while polling REST (REST responses never carry a
        // reliable "closed" flag; only the WebSocket's `x` field does).
        optional<candlesticks::Candlestick> lastCandle;

        auto ingestCandle = [&](const candlesticks::Candlestick& candle) {
            if (candle.closed) {
                // WebSocket told us directly: this candle just closed.
                const auto snapshot = writeClosedCandleRow(csv, candle, engine, vizServer, symbol, label);
                // SMC/ICT live tap point. Deliberately update()d here, not
                // peek()d - see every smc-ict/ concept file's own header
                // comment on why these structures never get a live/forming
                // preview the way indicators do. Only broadcast (and the
                // snapshot only rebuilt) when something actually changed -
                // see SmcUpdate::empty() in smc_engine.h.
                auto smcUpdate = smcEngine.update(candle, snapshot.atr);
                if (!smcUpdate.empty()) {
                    vizServer.pushSmcEvent(symbol, label, smcUpdate);
                    vizServer.setSmcSnapshot(symbol, label, smcEngine.currentSnapshot());
                }
                lastCandle.reset();
                board.updateLine(task.rowIndex, "last candle saved to CSV");
                return;
            }
            if (lastCandle.has_value() && lastCandle->openTime != candle.openTime) {
                // Rollover detected between two REST polls / forming ticks.
                candlesticks::Candlestick finished = *lastCandle;
                finished.closed = true;
                const auto snapshot = writeClosedCandleRow(csv, finished, engine, vizServer, symbol, label);
                auto smcUpdate = smcEngine.update(finished, snapshot.atr);
                if (!smcUpdate.empty()) {
                    vizServer.pushSmcEvent(symbol, label, smcUpdate);
                    vizServer.setSmcSnapshot(symbol, label, smcEngine.currentSnapshot());
                }
            }
            lastCandle = candle;
            renderLine(candle, "FORMING");
        };

        try {
            if (auto candle = fetchCurrentCandle(ioc, ctx, symbol, task.binanceInterval)) {
                ingestCandle(*candle);
            }
        } catch (const exception& e) {
            board.log("[" + label + "] Initial REST fetch failed: " + e.what());
        }

        net::steady_timer pollTimer(ioc);
        bool usingRestFallback = false;
        function<void(beast::error_code)> pollHandler;
        pollHandler = [&](beast::error_code ec) {
            if (ec) return;
            try {
                const auto candle = fetchCurrentCandle(ioc, ctx, symbol, task.binanceInterval);
                if (candle.has_value()) {
                    ingestCandle(*candle);
                }
            } catch (const exception& e) {
                board.log("[" + label + "] REST error: " + e.what());
            }
            pollTimer.expires_after(chrono::seconds(1));
            pollTimer.async_wait(pollHandler);
        };

        net::steady_timer wsReconnectTimer(ioc);
        net::steady_timer wsWatchdogTimer(ioc);
        auto lastWsMessage = chrono::steady_clock::now();
        bool wsEverConnected = false;
        int consecutiveWsFailures = 0;
        constexpr int kMaxWsFailuresBeforeFallback = 5;

        function<void()> startWsAttempt;

        auto beginRestFallback = [&]() {
            if (usingRestFallback) return;
            usingRestFallback = true;
            board.log("[" + label + "] giving up after " + to_string(consecutiveWsFailures) +
                      " failed attempts; falling back to REST polling.");
            pollHandler({});
        };

        auto scheduleWsReconnect = [&]() {
            ++consecutiveWsFailures;
            if (consecutiveWsFailures >= kMaxWsFailuresBeforeFallback) {
                beginRestFallback();
                return;
            }
            const auto backoff = chrono::seconds(min(1 << consecutiveWsFailures, 30));
            board.log("[" + label + "] reconnecting in " + to_string(backoff.count()) +
                      "s (attempt " + to_string(consecutiveWsFailures) + ")");
            wsReconnectTimer.expires_after(backoff);
            wsReconnectTimer.async_wait([&](beast::error_code ec) { if (!ec) startWsAttempt(); });
        };

        startWsAttempt = [&]() {
            if (usingRestFallback) return;
            const string target = "/market/ws/" + toLower(symbol) + "@kline_" + task.binanceInterval;
            auto session = make_shared<KlineStreamSession>(ioc, ctx, target,
                [&](const candlesticks::Candlestick& candle) {
                    lastWsMessage = chrono::steady_clock::now();
                    wsEverConnected = true;
                    consecutiveWsFailures = 0;
                    ingestCandle(candle);
                },
                [&]() { /* no-op: KlineStreamSession already logs the connect event */ },
                [&](const char* step, beast::error_code ec) {
                    board.log("[" + label + "] " + step + " failed: " + ec.message());
                    scheduleWsReconnect();
                },
                [&](const string& msg) { board.log("[" + label + "] " + msg); });
            session->run();
        };

        // Defense-in-depth: catches a connection that goes silent without
        // surfacing an error (the exact failure mode from earlier debugging).
        function<void(beast::error_code)> watchdogTick = [&](beast::error_code ec) {
            if (ec || usingRestFallback) return;
            const auto silentFor = chrono::steady_clock::now() - lastWsMessage;
            const auto limit = wsEverConnected ? chrono::seconds(60) : chrono::seconds(10);
            if (silentFor > limit) {
                board.log("[" + label + "] no data for " +
                          to_string(chrono::duration_cast<chrono::seconds>(silentFor).count()) +
                          "s; treating connection as dead.");
                lastWsMessage = chrono::steady_clock::now();
                scheduleWsReconnect();
            }
            wsWatchdogTimer.expires_after(chrono::seconds(5));
            wsWatchdogTimer.async_wait(watchdogTick);
        };

        startWsAttempt();
        wsWatchdogTimer.expires_after(chrono::seconds(5));
        wsWatchdogTimer.async_wait(watchdogTick);
        ioc.run();
    } catch (const exception& e) {
        board.log("[" + label + "] Fatal error: " + string(e.what()));
    }
}

}  // namespace binance