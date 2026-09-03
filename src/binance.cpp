#include "candlesticks.h"

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
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using namespace std;

namespace {

constexpr auto kHost = "fapi.binance.com";
constexpr auto kPort = "443";

string trim(const string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

string toUpper(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(toupper(c)); });
    return value;
}

string toLower(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return value;
}

string formatCurrentTime() {
    const auto now = chrono::system_clock::now();
    const auto epochSeconds = chrono::system_clock::to_time_t(now);
    tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &epochSeconds);
#else
    localtime_r(&epochSeconds, &localTm);
#endif
    ostringstream oss;
    oss << put_time(&localTm, "%H:%M:%S");
    return oss.str();
}

class CandlestickDisplay {
public:
    explicit CandlestickDisplay(string timeframe)
        : timeframe_(move(timeframe)), useAnsiLineClear_(enableAnsiLineClear()) {}

    void updateForming(const candlesticks::Candlestick& candle) {
        render(candle, "FORMING", false);
    }

    void commitClosed(const candlesticks::Candlestick& candle) {
        render(candle, "CLOSED", true);
    }

private:
    static bool enableAnsiLineClear() {
#if defined(_WIN32)
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode)) {
            return false;
        }
        return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
               SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        return true;
#endif
    }

    static size_t terminalColumns() {
#if defined(_WIN32)
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO bufferInfo{};
        if (output != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(output, &bufferInfo)) {
            return static_cast<size_t>(bufferInfo.srWindow.Right - bufferInfo.srWindow.Left + 1);
        }
#endif
        return 0;
    }

    static string formatPrice(double price) {
        ostringstream oss;
        oss << setprecision(10) << defaultfloat << price;
        return oss.str();
    }

    static const char* trendCode(candlesticks::Trend trend) {
        switch (trend) {
            case candlesticks::Trend::Bullish:
                return "B";
            case candlesticks::Trend::Bearish:
                return "S";
            case candlesticks::Trend::Neutral:
                return "N";
        }
        return "?";
    }

    string formatLine(const candlesticks::Candlestick& candle, const char* state) const {
        const string open = formatPrice(candle.open);
        const string high = formatPrice(candle.high);
        const string low = formatPrice(candle.low);
        const string close = formatPrice(candle.close);
        const auto trend = candlesticks::trendOf(candle);
        const string fullLine = candle.symbol + " " + timeframe_ + " O:" + open + " H:" +
                                high + " L:" + low + " C:" + close + " " +
                                candlesticks::trendLabel(trend) + " " + state;
        const size_t columns = terminalColumns();
        if (columns == 0 || fullLine.size() < columns) {
            return fullLine;
        }

        const string compactLine = "O:" + open + " H:" + high + " L:" + low + " C:" +
                                   close + " " + trendCode(trend) + " " +
                                   (state[0] == 'F' ? "F" : "C");
        return compactLine;
    }

    void render(const candlesticks::Candlestick& candle, const char* state, bool commitLine) {
        const string line = formatLine(candle, state);
        cout << '\r';
        if (useAnsiLineClear_) {
            cout << "\x1b[2K";
        }
        cout << line;
        if (!useAnsiLineClear_ && liveLineWidth_ > line.size()) {
            cout << string(liveLineWidth_ - line.size(), ' ');
        }
        if (commitLine) {
            cout << '\n';
            liveLineWidth_ = 0;
        } else {
            liveLineWidth_ = line.size();
        }
        cout << flush;
    }

    string timeframe_;
    bool useAnsiLineClear_;
    size_t liveLineWidth_ = 0;
};

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

constexpr auto kWsHost = "fstream.binance.com";
constexpr auto kWsPort = "443";

class KlineStreamSession : public std::enable_shared_from_this<KlineStreamSession> {
public:
    using CandleHandler = function<void(const candlesticks::Candlestick&)>;
        using ConnectedHandler = function<void()>;
    using ErrorHandler = function<void(const char* /*step*/, beast::error_code)>;

    KlineStreamSession(net::io_context& ioc, ssl::context& ctx, string target,
                                                CandleHandler onCandle, ConnectedHandler onConnected,
                                                ErrorHandler onError)
        : resolver_(net::make_strand(ioc)),
          ws_(net::make_strand(ioc), ctx),
          target_(move(target)),
          onCandle_(move(onCandle)),
                    onConnected_(move(onConnected)),
          onError_(move(onError)) {
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
        cerr << "[ws] connected: " << target_ << endl;
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
            cerr << "[ws] parse error: " << e.what() << endl;
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
};

}  // namespace

int main() {
    cout << "Enter a Binance USD-M Futures symbol (e.g. BTCUSDT): ";
    string rawSymbol;
    if (!getline(cin, rawSymbol)) {
        cerr << "No input provided." << endl;
        return 1;
    }

    const string symbol = toUpper(trim(rawSymbol));
    if (symbol.empty()) {
        cerr << "Symbol must not be empty." << endl;
        return 1;
    }

    cout << "Enter a candlestick timeframe (e.g. 1m, 15m, 1h, 4h, 1d, 1w, 1mo): ";
    string rawTimeframe;
    if (!getline(cin, rawTimeframe)) {
        cerr << "No timeframe provided." << endl;
        return 1;
    }

    const string timeframe = trim(rawTimeframe);
    const auto binanceInterval = candlesticks::toBinanceInterval(timeframe);
    if (!binanceInterval.has_value()) {
        cerr << "Unsupported timeframe. Choose one of: "
             << candlesticks::supportedTimeframes() << endl;
        return 1;
    }

    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.load_verify_file("C:/certs/cacert.pem");
        ctx.set_verify_mode(ssl::verify_peer);

           cout << "Polling " << symbol << " " << timeframe
               << " candlesticks. Press Ctrl+C to exit.\n";

        optional<candlesticks::Candlestick> latestCandle;
        CandlestickDisplay candleDisplay(timeframe);
        bool displayReady = false;

        auto ingestCandle = [&](const candlesticks::Candlestick& candle) {
            if (latestCandle.has_value() && latestCandle->openTime != candle.openTime) {
                latestCandle->closed = true;
                if (displayReady) {
                    candleDisplay.commitClosed(*latestCandle);
                }
            }
            latestCandle = candle;
            if (displayReady) {
                candleDisplay.updateForming(candle);
            }
        };

        try {
            if (auto candle = fetchCurrentCandle(ioc, ctx, symbol, *binanceInterval)) {
                ingestCandle(*candle);
            }
        } catch (const exception& e) {
            cerr << "Initial REST fetch failed: " << e.what() << endl;
        }

        net::steady_timer pollTimer(ioc);
        function<void(beast::error_code)> pollHandler;
        pollHandler = [&](beast::error_code ec) {
            if (ec) {
                return;
            }

            try {
                const auto candle = fetchCurrentCandle(ioc, ctx, symbol, *binanceInterval);
                if (candle.has_value()) {
                    ingestCandle(*candle);
                }
            } catch (const exception& e) {
                cerr << "Error retrieving candle: " << e.what() << endl;
            }

            pollTimer.expires_after(chrono::seconds(1));
            pollTimer.async_wait(pollHandler);
        };

        net::steady_timer wsReconnectTimer(ioc);
        net::steady_timer wsWatchdogTimer(ioc);
        auto lastWsMessage = chrono::steady_clock::now();
        bool wsEverConnected = false;
        int consecutiveWsFailures = 0;
        bool usingRestFallback = false;
        constexpr int kMaxWsFailuresBeforeFallback = 5;

        function<void()> startWsAttempt;

        auto beginRestFallback = [&]() {
            if (usingRestFallback) return;
            usingRestFallback = true;
            cerr << "[ws] giving up after " << consecutiveWsFailures
                 << " failed attempts; falling back to REST polling." << endl;
            displayReady = true;
            if (latestCandle.has_value()) {
                candleDisplay.updateForming(*latestCandle);
            }
            pollHandler({});
        };

        auto scheduleWsReconnect = [&]() {
            ++consecutiveWsFailures;
            if (consecutiveWsFailures >= kMaxWsFailuresBeforeFallback) {
                beginRestFallback();
                return;
            }
            const auto backoff = chrono::seconds(min(1 << consecutiveWsFailures, 30));
            cerr << "[ws] reconnecting in " << backoff.count()
                 << "s (attempt " << consecutiveWsFailures << ")" << endl;
            wsReconnectTimer.expires_after(backoff);
            wsReconnectTimer.async_wait([&](beast::error_code ec) { if (!ec) startWsAttempt(); });
        };

        startWsAttempt = [&]() {
            if (usingRestFallback) return;
            const string target = "/market/ws/" + toLower(symbol) + "@kline_" + *binanceInterval;
            auto session = make_shared<KlineStreamSession>(ioc, ctx, target,
                [&](const candlesticks::Candlestick& candle) {
                    lastWsMessage = chrono::steady_clock::now();
                    wsEverConnected = true;
                    consecutiveWsFailures = 0;
                    ingestCandle(candle);
                },
                [&]() {
                    displayReady = true;
                    if (latestCandle.has_value()) {
                        candleDisplay.updateForming(*latestCandle);
                    }
                },
                [&](const char* step, beast::error_code ec) {
                    cerr << "[ws] " << step << " failed: " << ec.message() << endl;
                    scheduleWsReconnect();
                });
            session->run();
        };

        // Defense-in-depth: catches a connection that goes silent without
        // surfacing an error (the exact failure mode from earlier debugging).
        function<void(beast::error_code)> watchdogTick = [&](beast::error_code ec) {
            if (ec || usingRestFallback) return;
            const auto silentFor = chrono::steady_clock::now() - lastWsMessage;
            const auto limit = wsEverConnected ? chrono::seconds(60) : chrono::seconds(10);
            if (silentFor > limit) {
                cerr << "[ws] no data for " << chrono::duration_cast<chrono::seconds>(silentFor).count()
                     << "s; treating connection as dead." << endl;
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
        cerr << "\nError: " << e.what() << endl;
        return 1;
    }
}
