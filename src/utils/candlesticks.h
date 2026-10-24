#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

using namespace std;

namespace candlesticks {

struct Candlestick {
    string symbol;
    int64_t openTime;
    int64_t closeTime;
    double open;
    double high;
    double low;
    double close;
    double baseVolume;          // Index 5 (REST) / "v" (WebSocket): total base-asset volume traded in this candle.
    double quoteVolume;         // Index 7 (REST) / "q" (WebSocket): total quote-asset (e.g. USDT) value of those trades.
    double takerBuyBaseVolume;  // Index 9 (REST) / "V" (WebSocket): base-asset volume bought via market (taker) orders.
    double takerBuyQuoteVolume; // Index 10 (REST) / "Q" (WebSocket): quote-asset value of taker buy volume.
    bool closed;
};

enum class Trend {
    Bullish,
    Bearish,
    Neutral,
};

optional<string> toBinanceInterval(const string& timeframe);
string supportedTimeframes();
optional<Candlestick> parseKlineMessage(const nlohmann::json& message);
optional<Candlestick> parseKlineResponse(const nlohmann::json& response,
                                         const string& symbol);
Trend trendOf(const Candlestick& candle);
const char* trendLabel(Trend trend);

}  // namespace candlesticks