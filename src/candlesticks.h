#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace candlesticks {

struct Candlestick {
    std::string symbol;
    std::int64_t openTime;
    std::int64_t closeTime;
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

std::optional<std::string> toBinanceInterval(const std::string& timeframe);
std::string supportedTimeframes();
std::optional<Candlestick> parseKlineMessage(const nlohmann::json& message);
std::optional<Candlestick> parseKlineResponse(const nlohmann::json& response,
                                              const std::string& symbol);
Trend trendOf(const Candlestick& candle);
const char* trendLabel(Trend trend);

}  // namespace candlesticks