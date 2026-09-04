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

namespace {

constexpr auto kHost = "fapi.binance.com";
constexpr auto kPort = "443";
constexpr auto kWsHost = "fstream.binance.com";
constexpr auto kWsPort = "443";

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

// ---------------------------------------------------------------------------
// TerminalBoard: replaces CandlestickDisplay. The old class committed each
// closed candle as a permanent scrollback line; that model no longer applies
// now that closed candles live in CSV files instead. This board instead
// reserves one fixed, in-place-updating line per timeframe (top to bottom,
// in the order the user typed them) using ANSI cursor movement, guarded by a
// single mutex so multiple stream threads can never interleave writes.
//
// `log()` is the only other thing allowed to touch stdout/stderr after
// construction; it tracks how many extra lines have scrolled below the
// board so the cursor math for updateLine() stays correct indefinitely.
// candlesticks.h / candlesticks.cpp are not touched by any of this.
// ---------------------------------------------------------------------------
class TerminalBoard {
public:
    explicit TerminalBoard(vector<string> labels)
        : labels_(move(labels)), ansiEnabled_(enableAnsi()) {
        lock_guard<mutex> lock(mutex_);
        for (const auto& label : labels_) {
            cout << "[" << label << "] waiting for data...\n";
        }
        cout << flush;
    }

    void updateLine(size_t index, const string& text) {
        lock_guard<mutex> lock(mutex_);
        const string full = "[" + labels_[index] + "] " + text;
        if (!ansiEnabled_) {
            cout << full << '\n' << flush;
            return;
        }
        const size_t upCount = (labels_.size() - index) + extraLines_;
        cout << "\x1b[" << upCount << "A\r\x1b[2K" << full
             << "\x1b[" << upCount << "B\r" << flush;
    }

    void log(const string& text) {
        lock_guard<mutex> lock(mutex_);
        cerr << text << '\n' << flush;
        if (ansiEnabled_) {
            ++extraLines_;
        }
    }

private:
    static bool enableAnsi() {
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

    vector<string> labels_;
    bool ansiEnabled_;
    size_t extraLines_ = 0;
    mutex mutex_;
};

size_t terminalColumns() {
#if defined(_WIN32)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO bufferInfo{};
    if (output != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(output, &bufferInfo)) {
        return static_cast<size_t>(bufferInfo.srWindow.Right - bufferInfo.srWindow.Left + 1);
    }
#endif
    return 0;
}

string formatPrice(double price) {
    ostringstream oss;
    oss << setprecision(10) << defaultfloat << price;
    return oss.str();
}

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

string formatCandleLine(const string& symbol, const candlesticks::Candlestick& candle,
                        const char* stateTag, bool compact) {
    const string open = formatPrice(candle.open);
    const string high = formatPrice(candle.high);
    const string low = formatPrice(candle.low);
    const string close = formatPrice(candle.close);
    const auto trend = candlesticks::trendOf(candle);
    if (!compact) {
        return symbol + " O:" + open + " H:" + high + " L:" + low + " C:" + close + " " +
               candlesticks::trendLabel(trend) + " " + stateTag;
    }
    const char* code = trend == candlesticks::Trend::Bullish ? "B" :
                        trend == candlesticks::Trend::Bearish ? "S" : "N";
    return "O:" + open + " H:" + high + " L:" + low + " C:" + close + " " + code + " " +
           stateTag;
}

// ---------------------------------------------------------------------------
// Timeframe -> English CSV filename mapping
// ---------------------------------------------------------------------------
optional<string> englishNameForInterval(const string& binanceInterval) {
    static const unordered_map<string, string> kNames = {
        {"1m", "one_minute"},   {"3m", "three_minutes"}, {"5m", "five_minutes"},
        {"15m", "fifteen_minutes"}, {"30m", "thirty_minutes"}, {"1h", "one_hour"},
        {"2h", "two_hours"},    {"4h", "four_hours"},    {"6h", "six_hours"},
        {"8h", "eight_hours"},  {"12h", "twelve_hours"}, {"1d", "one_day"},
        {"3d", "three_days"},   {"1w", "one_week"},      {"1M", "one_month"},
    };
    const auto it = kNames.find(binanceInterval);
    if (it == kNames.end()) {
        return nullopt;
    }
    return it->second;
}

vector<string> splitByComma(const string& raw) {
    vector<string> parts;
    stringstream ss(raw);
    string item;
    while (getline(ss, item, ',')) {
        parts.push_back(item);
    }
    return parts;
}

struct TimeframeTask {
    string rawTimeframe;
    string binanceInterval;
    string englishName;
    size_t rowIndex = 0;
};

ofstream openCsvAppend(const filesystem::path& path) {
    const bool needsHeader = !filesystem::exists(path) || filesystem::file_size(path) == 0;
    ofstream csv(path, ios::app);
    if (!csv.is_open()) {
        throw runtime_error("Failed to open CSV file: " + path.string());
    }
    if (needsHeader) {
        csv << "timestamp,open,high,low,close\n";
        csv.flush();
    }
    return csv;
}

void writeClosedCandleRow(ofstream& csv, const candlesticks::Candlestick& candle) {
    csv << formatUtcTimestamp(candle.openTime) << ',' << formatPrice(candle.open) << ','
        << formatPrice(candle.high) << ',' << formatPrice(candle.low) << ','
        << formatPrice(candle.close) << '\n';
    csv.flush();
}

// ---------------------------------------------------------------------------
// One fully isolated worker per timeframe: own io_context/ssl::context, own
// CSV file, own REST-priming/WS-reconnect/watchdog/REST-fallback state. A
// failure or fallback on one timeframe cannot affect any other.
// ---------------------------------------------------------------------------
void runTimeframeStream(const string& symbol, TimeframeTask task, filesystem::path csvPath,
                        TerminalBoard& board) {
    const string label = task.rawTimeframe;
    try {
        ofstream csv = openCsvAppend(csvPath);

        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.load_verify_file("C:/certs/cacert.pem");
        ctx.set_verify_mode(ssl::verify_peer);

        auto renderLine = [&](const candlesticks::Candlestick& candle, const char* tag) {
            const size_t columns = terminalColumns();
            string line = formatCandleLine(symbol, candle, tag, false);
            if (columns > 0 && (label.size() + 3 + line.size()) >= columns) {
                line = formatCandleLine(symbol, candle, tag, true);
            }
            board.updateLine(task.rowIndex, line);
        };

        // Tracks the last candle seen from ANY source so we can detect a
        // rollover even while polling REST (REST responses never carry a
        // reliable "closed" flag; only the WebSocket's `x` field does).
        optional<candlesticks::Candlestick> lastCandle;

        auto ingestCandle = [&](const candlesticks::Candlestick& candle) {
            if (candle.closed) {
                // WebSocket told us directly: this candle just closed.
                writeClosedCandleRow(csv, candle);
                lastCandle.reset();
                board.updateLine(task.rowIndex, "last candle saved to CSV");
                return;
            }
            if (lastCandle.has_value() && lastCandle->openTime != candle.openTime) {
                // Rollover detected between two REST polls / forming ticks.
                candlesticks::Candlestick finished = *lastCandle;
                finished.closed = true;
                writeClosedCandleRow(csv, finished);
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

    cout << "Enter candlestick timeframes, comma-separated (e.g. 1m,15m,1h,1d): ";
    string rawTimeframes;
    if (!getline(cin, rawTimeframes)) {
        cerr << "No timeframe input provided." << endl;
        return 1;
    }

    vector<TimeframeTask> tasks;
    set<string> seenIntervals;
    for (const auto& piece : splitByComma(rawTimeframes)) {
        const string timeframe = trim(piece);
        if (timeframe.empty()) {
            continue;
        }
        const auto binanceInterval = candlesticks::toBinanceInterval(timeframe);
        if (!binanceInterval.has_value()) {
            cerr << "Skipping unsupported timeframe '" << timeframe << "'. Choose from: "
                 << candlesticks::supportedTimeframes() << endl;
            continue;
        }
        if (!seenIntervals.insert(*binanceInterval).second) {
            cerr << "Skipping duplicate timeframe '" << timeframe << "'." << endl;
            continue;
        }
        const auto englishName = englishNameForInterval(*binanceInterval);
        if (!englishName.has_value()) {
            cerr << "No CSV filename mapping for interval '" << *binanceInterval
                 << "'; skipping." << endl;
            continue;
        }

        TimeframeTask task;
        task.rawTimeframe = timeframe;
        task.binanceInterval = *binanceInterval;
        task.englishName = *englishName;
        task.rowIndex = tasks.size();
        tasks.push_back(move(task));
    }

    if (tasks.empty()) {
        cerr << "No valid timeframes provided. Exiting." << endl;
        return 1;
    }

    const string symbolFolder = toLower(symbol);
    error_code dirError;
    filesystem::create_directories(symbolFolder, dirError);
    if (dirError) {
        cerr << "Failed to create folder '" << symbolFolder << "': " << dirError.message()
             << endl;
        return 1;
    }

    cout << "Streaming " << symbol << " across " << tasks.size()
         << " timeframe(s). Closed candles are appended to CSVs under '" << symbolFolder
         << "/'. Press Ctrl+C to exit.\n";

    vector<string> labels;
    labels.reserve(tasks.size());
    for (const auto& task : tasks) {
        labels.push_back(task.rawTimeframe);
    }
    TerminalBoard board(labels);

    vector<thread> workers;
    workers.reserve(tasks.size());
    for (const auto& task : tasks) {
        filesystem::path csvPath = filesystem::path(symbolFolder) / (task.englishName + ".csv");
        workers.emplace_back(runTimeframeStream, symbol, task, move(csvPath), ref(board));
    }

    for (auto& worker : workers) {
        worker.join();
    }
}