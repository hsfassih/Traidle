#include "candlesticks.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>

namespace candlesticks {
namespace {

string trim(const string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool isSupportedBinanceInterval(const string& interval) {
    static const array<string, 15> kSupportedIntervals = {
        "1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h", "6h", "8h",
        "12h", "1d", "3d", "1w", "1M",
    };
    return find(kSupportedIntervals.begin(), kSupportedIntervals.end(), interval) !=
           kSupportedIntervals.end();
}

optional<string> stringField(const nlohmann::json& object, const char* key) {
    const auto field = object.find(key);
    if (field == object.end() || !field->is_string()) {
        return nullopt;
    }
    return field->get<string>();
}

optional<int64_t> integerField(const nlohmann::json& object, const char* key) {
    const auto field = object.find(key);
    if (field == object.end() ||
        (!field->is_number_integer() && !field->is_number_unsigned())) {
        return nullopt;
    }
    try {
        return field->get<int64_t>();
    } catch (const exception&) {
        return nullopt;
    }
}

optional<double> positivePriceField(const nlohmann::json& object, const char* key) {
    const auto price = stringField(object, key);
    if (!price.has_value()) {
        return nullopt;
    }

    try {
        const double value = stod(*price);
        if (!isfinite(value) || value <= 0.0) {
            return nullopt;
        }
        return value;
    } catch (const exception&) {
        return nullopt;
    }
}

optional<double> positivePriceElement(const nlohmann::json& values, size_t index) {
    if (index >= values.size() || !values[index].is_string()) {
        return nullopt;
    }

    try {
        const double value = stod(values[index].get<string>());
        if (!isfinite(value) || value <= 0.0) {
            return nullopt;
        }
        return value;
    } catch (const exception&) {
        return nullopt;
    }
}

}  // namespace

optional<string> toBinanceInterval(const string& rawTimeframe) {
    const string timeframe = trim(rawTimeframe);
    const auto unitStart = find_if(
        timeframe.begin(), timeframe.end(),
        [](unsigned char character) { return !isdigit(character); });

    if (unitStart == timeframe.begin() || unitStart == timeframe.end() ||
        !all_of(timeframe.begin(), unitStart,
                [](unsigned char character) { return isdigit(character); })) {
        return nullopt;
    }

    const string unit(unitStart, timeframe.end());
    const string interval = unit == "mo" ? string(timeframe.begin(), unitStart) + "M" : timeframe;
    if (!isSupportedBinanceInterval(interval)) {
        return nullopt;
    }
    return interval;
}

string supportedTimeframes() {
    return "1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 8h, 12h, 1d, 3d, 1w, 1mo";
}

optional<Candlestick> parseKlineMessage(const nlohmann::json& message) {
    if (!message.is_object()) {
        return nullopt;
    }

    const nlohmann::json* data = &message;
    const auto dataField = message.find("data");
    if (dataField != message.end()) {
        if (!dataField->is_object()) {
            return nullopt;
        }
        data = &*dataField;
    }

    const auto eventType = stringField(*data, "e");
    const auto klineField = data->find("k");
    if (!eventType.has_value() || *eventType != "kline" || klineField == data->end() ||
        !klineField->is_object()) {
        return nullopt;
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
        *high < max(*open, *close) || *low > min(*open, *close)) {
        return nullopt;
    }

    return Candlestick{*symbol, *openTime, *closeTime, *open, *high, *low, *close,
                       closedField->get<bool>()};
}

optional<Candlestick> parseKlineResponse(const nlohmann::json& response,
                                         const string& symbol) {
    if (!response.is_array() || response.empty() || !response.front().is_array()) {
        return nullopt;
    }

    const nlohmann::json& kline = response.front();
    if (kline.size() < 7 || !kline[0].is_number() || !kline[6].is_number()) {
        return nullopt;
    }

    const auto open = positivePriceElement(kline, 1);
    const auto high = positivePriceElement(kline, 2);
    const auto low = positivePriceElement(kline, 3);
    const auto close = positivePriceElement(kline, 4);
    if (!open.has_value() || !high.has_value() || !low.has_value() || !close.has_value() ||
        *high < max(*open, *close) || *low > min(*open, *close)) {
        return nullopt;
    }

    try {
        return Candlestick{symbol, kline[0].get<int64_t>(), kline[6].get<int64_t>(), *open,
                           *high, *low, *close, false};
    } catch (const exception&) {
        return nullopt;
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