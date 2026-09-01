#include "candlesticks.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>

namespace candlesticks {
namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool isSupportedBinanceInterval(const std::string& interval) {
    static const std::array<std::string, 15> kSupportedIntervals = {
        "1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h", "6h", "8h",
        "12h", "1d", "3d", "1w", "1M",
    };
    return std::find(kSupportedIntervals.begin(), kSupportedIntervals.end(), interval) !=
           kSupportedIntervals.end();
}

std::optional<std::string> stringField(const nlohmann::json& object, const char* key) {
    const auto field = object.find(key);
    if (field == object.end() || !field->is_string()) {
        return std::nullopt;
    }
    return field->get<std::string>();
}

std::optional<std::int64_t> integerField(const nlohmann::json& object, const char* key) {
    const auto field = object.find(key);
    if (field == object.end() ||
        (!field->is_number_integer() && !field->is_number_unsigned())) {
        return std::nullopt;
    }
    try {
        return field->get<std::int64_t>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> positivePriceField(const nlohmann::json& object, const char* key) {
    const auto price = stringField(object, key);
    if (!price.has_value()) {
        return std::nullopt;
    }

    try {
        const double value = std::stod(*price);
        if (!std::isfinite(value) || value <= 0.0) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> positivePriceElement(const nlohmann::json& values, std::size_t index) {
    if (index >= values.size() || !values[index].is_string()) {
        return std::nullopt;
    }

    try {
        const double value = std::stod(values[index].get<std::string>());
        if (!std::isfinite(value) || value <= 0.0) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::optional<std::string> toBinanceInterval(const std::string& rawTimeframe) {
    const std::string timeframe = trim(rawTimeframe);
    const auto unitStart = std::find_if(
        timeframe.begin(), timeframe.end(),
        [](unsigned char character) { return !std::isdigit(character); });

    if (unitStart == timeframe.begin() || unitStart == timeframe.end() ||
        !std::all_of(timeframe.begin(), unitStart,
                     [](unsigned char character) { return std::isdigit(character); })) {
        return std::nullopt;
    }

    const std::string unit(unitStart, timeframe.end());
    const std::string interval = unit == "mo"
                                     ? std::string(timeframe.begin(), unitStart) + "M"
                                     : timeframe;
    if (!isSupportedBinanceInterval(interval)) {
        return std::nullopt;
    }
    return interval;
}

std::string supportedTimeframes() {
    return "1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 8h, 12h, 1d, 3d, 1w, 1mo";
}

std::optional<Candlestick> parseKlineMessage(const nlohmann::json& message) {
    if (!message.is_object()) {
        return std::nullopt;
    }

    const nlohmann::json* data = &message;
    const auto dataField = message.find("data");
    if (dataField != message.end()) {
        if (!dataField->is_object()) {
            return std::nullopt;
        }
        data = &*dataField;
    }

    const auto eventType = stringField(*data, "e");
    const auto klineField = data->find("k");
    if (!eventType.has_value() || *eventType != "kline" || klineField == data->end() ||
        !klineField->is_object()) {
        return std::nullopt;
    }

    const nlohmann::json& kline = *klineField;
    const auto symbol = stringField(*data, "s");
    const auto openTime = integerField(kline, "t");
    const auto closeTime = integerField(kline, "T");
    const auto open = positivePriceField(kline, "o");
    const auto high = positivePriceField(kline, "h");
    const auto low = positivePriceField(kline, "l");
    const auto close = positivePriceField(kline, "c");
    const auto closedField = kline.find("x");

    if (!symbol.has_value() || !openTime.has_value() || !closeTime.has_value() ||
        !open.has_value() || !high.has_value() || !low.has_value() || !close.has_value() ||
        closedField == kline.end() || !closedField->is_boolean() ||
        *high < std::max(*open, *close) || *low > std::min(*open, *close)) {
        return std::nullopt;
    }

    return Candlestick{*symbol, *openTime, *closeTime, *open, *high, *low, *close,
                       closedField->get<bool>()};
}

std::optional<Candlestick> parseKlineResponse(const nlohmann::json& response,
                                              const std::string& symbol) {
    if (!response.is_array() || response.empty() || !response.front().is_array()) {
        return std::nullopt;
    }

    const nlohmann::json& kline = response.front();
    if (kline.size() < 7 || !kline[0].is_number() || !kline[6].is_number()) {
        return std::nullopt;
    }

    const auto open = positivePriceElement(kline, 1);
    const auto high = positivePriceElement(kline, 2);
    const auto low = positivePriceElement(kline, 3);
    const auto close = positivePriceElement(kline, 4);
    if (!open.has_value() || !high.has_value() || !low.has_value() || !close.has_value() ||
        *high < std::max(*open, *close) || *low > std::min(*open, *close)) {
        return std::nullopt;
    }

    try {
        return Candlestick{symbol, kline[0].get<std::int64_t>(),
                           kline[6].get<std::int64_t>(), *open, *high, *low, *close, false};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Trend trendOf(const Candlestick& candle) {
    if (candle.close > candle.open) {
        return Trend::Bullish;
    }
    if (candle.close < candle.open) {
        return Trend::Bearish;
    }
    return Trend::Neutral;
}

const char* trendLabel(Trend trend) {
    switch (trend) {
        case Trend::Bullish:
            return "BULLISH";
        case Trend::Bearish:
            return "BEARISH";
        case Trend::Neutral:
            return "NEUTRAL";
    }
    return "UNKNOWN";
}

}  // namespace candlesticks