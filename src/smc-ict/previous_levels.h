#pragma once

#include "../candlesticks.h"
#include "smc_types.h"

#include <optional>

using namespace std;

// ---------------------------------------------------------------------------
// previous_levels: PDH/PDL bucketed by NEW YORK calendar day (via ny_time.h),
// deliberately NOT the same "day" as indicators.h's existing UTC-midnight
// VWAP reset, and NOT the same as Binance's own UTC-aligned daily kline
// boundary either - see ict_smc_numerical_definitions.md section 7 for why
// a genuine PDH/PDL cannot just read the existing daily CSV directly and
// needs its own NY-day aggregation over whatever timeframe is actually
// streaming.
// ---------------------------------------------------------------------------
namespace smc {

class PreviousLevelsTracker {
public:
    // Feed one closed candle, in chronological order, from ANY timeframe.
    // Returns the finalized PDH/PDL for the NY day that just ended, but
    // ONLY on the candle that crosses into a new NY day (nullopt every
    // other candle) - that is the one moment "yesterday's" high/low is
    // actually final and worth reporting as an event.
    optional<PreviousDayLevels> update(const candlesticks::Candlestick& candle);

    // The most recently finalized PDH/PDL, valid at any time (not just on
    // the rollover candle) - what a snapshot/reconnect needs.
    optional<double> currentPdh() const { return lastFinalizedPdh_; }
    optional<double> currentPdl() const { return lastFinalizedPdl_; }

private:
    optional<int64_t> currentDayStartUtcMs_;
    double currentDayHigh_ = 0.0;
    double currentDayLow_ = 0.0;
    optional<double> lastFinalizedPdh_;
    optional<double> lastFinalizedPdl_;
};

}  // namespace smc
