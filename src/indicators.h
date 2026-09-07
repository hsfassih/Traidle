#pragma once

#include "candlesticks.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

using namespace std;

// ---------------------------------------------------------------------------
// indicators: a stateful, per-timeframe engine that turns each newly closed
// candle into a row of technical-analysis indicator values, in the exact
// order they belong in the CSV (between the volume columns and OHLC).
//
// One IndicatorEngine instance belongs to exactly one symbol/timeframe CSV,
// mirroring the existing one-thread-one-state-per-timeframe design in
// binance.cpp. Every indicator here needs some amount of price/volume
// history before it produces its first real value (a "warm-up" period);
// until then, update() leaves the corresponding field as nullopt, which the
// caller renders as a blank CSV cell - exactly like the blank cells already
// used elsewhere in this project for not-yet-available data.
//
// To survive a restart without losing that warm-up progress, every already
// -stored CSV row must be replayed back through update() (oldest to
// newest) before any new candle is processed. historical_get.cpp owns that
// replay, since it already owns CSV-reading logic; this module only knows
// how to consume one candle at a time and does not read or write files
// itself.
// ---------------------------------------------------------------------------
namespace indicators {

// One row's worth of indicator values. Every field is optional: nullopt
// means "still warming up", rendered as a blank CSV cell by
// formatIndicatorRow().
struct IndicatorSnapshot {
    optional<double> sma;             // SMA(20) of close
    optional<double> ema;             // EMA(20) of close
    optional<double> macdLine;        // EMA(12) - EMA(26) of close
    optional<double> macdSignal;      // EMA(9) of the MACD line
    optional<double> macdHistogram;   // macdLine - macdSignal
    optional<double> rsi;             // RSI(14), Wilder's smoothing
    optional<double> stochK;          // %K, 14-period
    optional<double> stochD;          // %D, 3-period SMA of %K
    optional<double> bbMiddle;        // SMA(20) of close (same window as `sma`)
    optional<double> bbUpper;         // bbMiddle + 2 * population stddev
    optional<double> bbLower;         // bbMiddle - 2 * population stddev
    optional<double> atr;             // ATR(14), Wilder's smoothing
    optional<double> vwap;            // Volume-weighted average price, resets each UTC day
    optional<double> obv;             // On-Balance Volume, cumulative from the first candle
};

// CSV header fragment for every indicator column, comma-separated and
// ending in a trailing comma, in the same fixed order as
// formatIndicatorRow() below. Meant to be concatenated directly between the
// volume columns' header and the OHLC columns' header.
string indicatorHeader();

// Formats one IndicatorSnapshot as a comma-separated, comma-terminated CSV
// fragment (a nullopt field renders as an empty cell), meant to be
// concatenated directly between the volume columns and the OHLC columns of
// a data row.
string formatIndicatorRow(const IndicatorSnapshot& snapshot);

// Stateful, single-threaded engine. Not thread-safe - each timeframe worker
// owns its own instance, exactly like it owns its own ofstream.
class IndicatorEngine {
public:
    IndicatorEngine();

    // Ingests one newly closed candle, in chronological order, and returns
    // its indicator values. Must be called for every closed candle exactly
    // once, in order - including during CSV replay after a restart.
    IndicatorSnapshot update(const candlesticks::Candlestick& candle);

private:
    // ---- Small reusable building blocks. Kept private/nested since they
    // are implementation details of this one engine, not a general-purpose
    // library. ----

    // Simple average of the first `period` samples to "seed" the value,
    // then a standard exponential recurrence with the supplied alpha.
    // Covers EMA(20), MACD's three internal EMAs (12/26/9), RSI's average
    // gain/loss (Wilder's smoothing, alpha = 1/period), and ATR (also
    // Wilder's smoothing) - only the alpha and the input samples differ.
    struct SeededEma {
        size_t period = 0;
        double alpha = 0.0;
        deque<double> seedWindow;
        double seedSum = 0.0;
        optional<double> value;

        void configure(size_t p, double a) {
            period = p;
            alpha = a;
        }

        optional<double> push(double sample) {
            if (value.has_value()) {
                value = sample * alpha + *value * (1.0 - alpha);
                return value;
            }
            seedWindow.push_back(sample);
            seedSum += sample;
            if (seedWindow.size() < period) {
                return nullopt;
            }
            value = seedSum / static_cast<double>(period);
            seedWindow.clear();
            return value;
        }
    };

    // Fixed-size trailing window of closes, tracked with a running sum and
    // sum-of-squares so mean/standard-deviation are O(1) per update
    // instead of rescanning the window every candle. Backs both the
    // SMA(20) column and the Bollinger middle/upper/lower bands - they are
    // the same 20-period window and the same mean.
    struct RollingMeanWindow {
        size_t period = 0;
        deque<double> values;
        double sum = 0.0;
        double sumSq = 0.0;

        void configure(size_t p) { period = p; }

        void push(double sample) {
            values.push_back(sample);
            sum += sample;
            sumSq += sample * sample;
            if (values.size() > period) {
                const double removed = values.front();
                values.pop_front();
                sum -= removed;
                sumSq -= removed * removed;
            }
        }

        bool ready() const { return values.size() == period; }
        double mean() const { return sum / static_cast<double>(period); }

        // Population standard deviation (divide by n, not n-1) - the
        // convention Bollinger's own formula and most charting platforms
        // use by default. Guarded against a tiny negative variance from
        // floating-point cancellation when the window is nearly flat.
        double populationStdDev() const {
            const double m = mean();
            const double variance = sumSq / static_cast<double>(period) - m * m;
            return variance > 0.0 ? sqrt(variance) : 0.0;
        }
    };

    // Trailing window of (high, low) pairs for the Stochastic Oscillator's
    // rolling highest-high / lowest-low. Rescans the window on every push
    // rather than maintaining an incremental running max/min - simpler to
    // get right, and cheap at period 14.
    struct HighLowWindow {
        size_t period = 0;
        deque<double> highs;
        deque<double> lows;

        void configure(size_t p) { period = p; }

        void push(double high, double low) {
            highs.push_back(high);
            lows.push_back(low);
            if (highs.size() > period) {
                highs.pop_front();
                lows.pop_front();
            }
        }

        bool ready() const { return highs.size() == period; }
        double highest() const { return *max_element(highs.begin(), highs.end()); }
        double lowest() const { return *min_element(lows.begin(), lows.end()); }
    };

    // Trailing simple-moving-average window - used for %D (a 3-period SMA
    // of %K).
    struct SimpleMovingWindow {
        size_t period = 0;
        deque<double> values;
        double sum = 0.0;

        void configure(size_t p) { period = p; }

        optional<double> push(double sample) {
            values.push_back(sample);
            sum += sample;
            if (values.size() > period) {
                sum -= values.front();
                values.pop_front();
            }
            if (values.size() < period) {
                return nullopt;
            }
            return sum / static_cast<double>(period);
        }
    };

    static constexpr size_t kSmaPeriod = 20;
    static constexpr size_t kEmaPeriod = 20;
    static constexpr size_t kMacdFastPeriod = 12;
    static constexpr size_t kMacdSlowPeriod = 26;
    static constexpr size_t kMacdSignalPeriod = 9;
    static constexpr size_t kRsiPeriod = 14;
    static constexpr size_t kStochPeriod = 14;
    static constexpr size_t kStochDPeriod = 3;
    static constexpr size_t kAtrPeriod = 14;
    static constexpr double kBollingerMultiplier = 2.0;
    static constexpr int64_t kMillisecondsPerDay = 24LL * 60 * 60 * 1000;

    RollingMeanWindow closeWindow20_;
    SeededEma ema20_;
    SeededEma macdFastEma_;
    SeededEma macdSlowEma_;
    SeededEma macdSignalEma_;
    optional<double> previousCloseForRsi_;
    SeededEma rsiAvgGain_;
    SeededEma rsiAvgLoss_;
    HighLowWindow highLowWindow14_;
    SimpleMovingWindow stochDWindow_;
    optional<double> previousCloseForAtr_;
    SeededEma atr_;
    optional<int64_t> currentVwapDay_;
    double vwapCumulativePriceVolume_ = 0.0;
    double vwapCumulativeVolume_ = 0.0;
    optional<double> previousCloseForObv_;
    double obv_ = 0.0;
};

}  // namespace indicators