#include <boost/asio/connect.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <functional>
#include <sstream>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using WebSocketStream = websocket::stream<ssl::stream<tcp::socket>>;
using namespace std;

namespace {

constexpr auto kHost = "fstream.binance.com";
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

bool isPositivePrice(const string& value) {
    try {
        return !value.empty() && isfinite(stod(value)) && stod(value) > 0.0;
    } catch (const exception&) {
        return false;
    }
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

    const string streamName = toLower(symbol) + "@trade";
    const string target = "/ws/" + streamName;

    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.load_verify_file("C:/certs/cacert.pem");
        ctx.set_verify_mode(ssl::verify_peer);

        WebSocketStream ws(ioc, ctx);

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), kHost)) {
            throw beast::system_error{beast::error_code(
                static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
        }
        ws.next_layer().set_verify_callback(ssl::host_name_verification(kHost));

        tcp::resolver resolver(ioc);
        const auto results = resolver.resolve(kHost, kPort);
        const auto endpoint = net::connect(beast::get_lowest_layer(ws), results);

        ws.next_layer().handshake(ssl::stream_base::client);

        const string hostHeader = string(kHost) + ":" + to_string(endpoint.port());
        ws.handshake(hostHeader, target);

        cout << "Streaming " << symbol << " last trade price. Press Ctrl+C to exit.\n";

        string latestSymbol = symbol;
        string latestPrice;
        net::steady_timer displayTimer(ioc);
        beast::flat_buffer buffer;
        string errorMessage;
        function<void(beast::error_code, size_t)> readHandler;
        function<void(beast::error_code)> displayHandler;

        readHandler = [&](beast::error_code ec, size_t) {
            if (ec) {
                errorMessage = "WebSocket read failed: " + ec.message();
                ioc.stop();
                return;
            }

            const string payload = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            try {
                const json message = json::parse(payload);
                if (message.contains("error")) {
                    errorMessage = "Binance reported an error for " + symbol + ": " +
                                   message["error"].dump();
                    ioc.stop();
                    return;
                }

                const json& data = message.contains("data") ? message["data"] : message;
                if (data.value("e", "") == "trade") {
                    const string candidatePrice = data.value("p", "");
                    if (isPositivePrice(candidatePrice)) {
                        latestSymbol = data.value("s", symbol);
                        latestPrice = candidatePrice;
                    }
                }
            } catch (const json::parse_error& e) {
                errorMessage = "Received malformed message from Binance: " + string(e.what());
                ioc.stop();
                return;
            }

            ws.async_read(buffer, readHandler);
        };

        displayHandler = [&](beast::error_code ec) {
            if (ec == net::error::operation_aborted) {
                return;
            }
            if (ec) {
                errorMessage = "Display timer failed: " + ec.message();
                ioc.stop();
                return;
            }
            if (!latestPrice.empty()) {
                cout << "\r" << latestSymbol << "  " << latestPrice << "  ["
                     << formatCurrentTime() << "]      " << flush;
            }
            displayTimer.expires_after(chrono::seconds(1));
            displayTimer.async_wait(displayHandler);
        };

        ws.async_read(buffer, readHandler);
        displayTimer.expires_after(chrono::seconds(1));
        displayTimer.async_wait(displayHandler);
        ioc.run();

        if (!errorMessage.empty()) {
            cerr << "\nError: " << errorMessage << endl;
            return 1;
        }
    } catch (const exception& e) {
        cerr << "\nError: " << e.what() << endl;
        return 1;
    }
}
