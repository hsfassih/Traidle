#include "candlesticks.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
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
#include <optional>
#include <sstream>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
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
        net::steady_timer pollTimer(ioc);
        function<void(beast::error_code)> pollHandler;
        pollHandler = [&](beast::error_code ec) {
            if (ec) {
                return;
            }

            try {
                const auto candle = fetchCurrentCandle(ioc, ctx, symbol, *binanceInterval);
                if (candle.has_value()) {
                    if (latestCandle.has_value() &&
                        latestCandle->openTime != candle->openTime) {
                        latestCandle->closed = true;
                        candleDisplay.commitClosed(*latestCandle);
                    }
                    latestCandle = candle;
                    candleDisplay.updateForming(*candle);
                }
            } catch (const exception& e) {
                cerr << "Error retrieving candle: " << e.what() << endl;
            }

            pollTimer.expires_after(chrono::seconds(1));
            pollTimer.async_wait(pollHandler);
        };

        pollHandler({});
        ioc.run();
    } catch (const exception& e) {
        cerr << "\nError: " << e.what() << endl;
        return 1;
    }
}
