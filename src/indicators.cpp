#include "indicators.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace indicators {
namespace {

string formatCell(const optional<double>& value) {
    if (!value.has_value()) {
        return string();
    }
    ostringstream oss;
    oss << setprecision(10) << defaultfloat << *value;
    return oss.str();
}

}  // namespace

string indicatorHeader() {
    return "sma,ema,macd_line,macd_signal,macd_histogram,rsi,stoch_k,stoch_d,"
           "bb_middle,bb_upper,bb_lower,atr,vwap,obv,";
}

string formatIndicatorRow(const IndicatorSnapshot& snapshot) {
    ostringstream oss;
    oss << formatCell(snapshot.sma) << ','
        << formatCell(snapshot.ema) << ','
        << formatCell(snapshot.macdLine) << ','
        << formatCell(snapshot.macdSignal) << ','
        << formatCell(snapshot.macdHistogram) << ','
        << formatCell(snapshot.rsi) << ','
        << formatCell(snapshot.stochK) << ','
        << formatCell(snapshot.stochD) << ','
        << formatCell(snapshot.bbMiddle) << ','
        << formatCell(snapshot.bbUpper) << ','
        << formatCell(snapshot.bbLower) << ','
        << formatCell(snapshot.atr) << ','
        << formatCell(snapshot.vwap) << ','
        << formatCell(snapshot.obv) << ',';
    return oss.str();
}

IndicatorEngine::IndicatorEngine() {
    closeWindow20_.configure(kSmaPeriod);
    ema20_.configure(kEmaPeriod, 2.0 / (static_cast<double>(kEmaPeriod) + 1.0));
    macdFastEma_.configure(kMacdFastPeriod, 2.0 / (static_cast<double>(kMacdFastPeriod) + 1.0));
    macdSlowEma_.configure(kMacdSlowPeriod, 2.0 / (static_cast<double>(kMacdSlowPeriod) + 1.0));
    macdSignalEma_.configure(kMacdSignalPeriod,
                             2.0 / (static_cast<double>(kMacdSignalPeriod) + 1.0));
    rsiAvgGain_.configure(kRsiPeriod, 1.0 / static_cast<double>(kRsiPeriod));
    rsiAvgLoss_.configure(kRsiPeriod, 1.0 / static_cast<double>(kRsiPeriod));
    highLowWindow14_.configure(kStochPeriod);
    stochDWindow_.configure(kStochDPeriod);
    atr_.configure(kAtrPeriod, 1.0 / static_cast<double>(kAtrPeriod));
}

IndicatorSnapshot IndicatorEngine::update(const candlesticks::Candlestick& candle) {
    IndicatorSnapshot snapshot;

    // ---- SMA(20) + Bollinger Bands(20, 2 sigma) - same rolling window ----
    closeWindow20_.push(candle.close);
    if (closeWindow20_.ready()) {
        const double mean = closeWindow20_.mean();
        const double stdDev = closeWindow20_.populationStdDev();
        snapshot.sma = mean;
        snapshot.bbMiddle = mean;
        snapshot.bbUpper = mean + kBollingerMultiplier * stdDev;
        snapshot.bbLower = mean - kBollingerMultiplier * stdDev;
    }

    // ---- EMA(20) ----
    snapshot.ema = ema20_.push(candle.close);

    // ---- MACD(12, 26, 9) ----
    const auto fastEma = macdFastEma_.push(candle.close);
    const auto slowEma = macdSlowEma_.push(candle.close);
    if (fastEma.has_value() && slowEma.has_value()) {
        const double macdLine = *fastEma - *slowEma;
        snapshot.macdLine = macdLine;
        const auto signal = macdSignalEma_.push(macdLine);
        if (signal.has_value()) {
            snapshot.macdSignal = signal;
            snapshot.macdHistogram = macdLine - *signal;
        }
    }

    // ---- RSI(14), Wilder's smoothing ----
    if (previousCloseForRsi_.has_value()) {
        const double change = candle.close - *previousCloseForRsi_;
        const double gain = change > 0.0 ? change : 0.0;
        const double loss = change < 0.0 ? -change : 0.0;
        const auto avgGain = rsiAvgGain_.push(gain);
        const auto avgLoss = rsiAvgLoss_.push(loss);
        if (avgGain.has_value() && avgLoss.has_value()) {
            if (*avgLoss == 0.0) {
                snapshot.rsi = 100.0;
            } else {
                const double rs = *avgGain / *avgLoss;
                snapshot.rsi = 100.0 - (100.0 / (1.0 + rs));
            }
        }
    }
    previousCloseForRsi_ = candle.close;

    // ---- Stochastic(14), %D = 3-period SMA of %K ----
    highLowWindow14_.push(candle.high, candle.low);
    if (highLowWindow14_.ready()) {
        const double highest = highLowWindow14_.highest();
        const double lowest = highLowWindow14_.lowest();
        const double range = highest - lowest;
        // A zero range means price hasn't moved at all across the whole
        // 14-period lookback, so %K is mathematically undefined (0/0).
        // Treated as neutral (50) rather than propagating a NaN/inf.
        const double percentK = range > 0.0 ? ((candle.close - lowest) / range) * 100.0 : 50.0;
        snapshot.stochK = percentK;
        snapshot.stochD = stochDWindow_.push(percentK);
    }

    // ---- ATR(14), Wilder's smoothing ----
    double trueRange;
    if (previousCloseForAtr_.has_value()) {
        trueRange = max({candle.high - candle.low,
                        abs(candle.high - *previousCloseForAtr_),
                        abs(candle.low - *previousCloseForAtr_)});
    } else {
        // No previous close yet (the very first candle this engine has
        // ever seen) - fall back to the high/low range alone, the
        // standard convention for an unavailable prior close.
        trueRange = candle.high - candle.low;
    }
    snapshot.atr = atr_.push(trueRange);
    previousCloseForAtr_ = candle.close;

    // ---- VWAP, reset at every UTC day boundary ----
    const int64_t day = candle.openTime / kMillisecondsPerDay;
    if (!currentVwapDay_.has_value() || *currentVwapDay_ != day) {
        currentVwapDay_ = day;
        vwapCumulativePriceVolume_ = 0.0;
        vwapCumulativeVolume_ = 0.0;
    }
    const double typicalPrice = (candle.high + candle.low + candle.close) / 3.0;
    vwapCumulativePriceVolume_ += typicalPrice * candle.baseVolume;
    vwapCumulativeVolume_ += candle.baseVolume;
    snapshot.vwap = vwapCumulativeVolume_ > 0.0
                        ? vwapCumulativePriceVolume_ / vwapCumulativeVolume_
                        : typicalPrice;

    // ---- OBV: cumulative, defined from the very first candle (no warm-up) ----
    if (previousCloseForObv_.has_value()) {
        if (candle.close > *previousCloseForObv_) {
            obv_ += candle.baseVolume;
        } else if (candle.close < *previousCloseForObv_) {
            obv_ -= candle.baseVolume;
        }
    }
    previousCloseForObv_ = candle.close;
    snapshot.obv = obv_;

    return snapshot;
}

}  // namespace indicators