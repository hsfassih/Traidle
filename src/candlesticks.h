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