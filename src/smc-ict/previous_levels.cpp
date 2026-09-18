#include "previous_levels.h"

#include "ny_time.h"

#include <algorithm>

namespace smc {

optional<PreviousDayLevels> PreviousLevelsTracker::update(const candlesticks::Candlestick& candle) {
    const int64_t dayStart = newYorkDayStartUtcMs(candle.openTime);

    if (!currentDayStartUtcMs_.has_value()) {
        currentDayStartUtcMs_ = dayStart;
        currentDayHigh_ = candle.high;
        currentDayLow_ = candle.low;
        return nullopt;
    }

    if (dayStart == *currentDayStartUtcMs_) {
        currentDayHigh_ = max(currentDayHigh_, candle.high);
        currentDayLow_ = min(currentDayLow_, candle.low);
        return nullopt;
    }

    // Day rolled over: finalize what just ended, then reset for the new day.
    PreviousDayLevels finalized;
    finalized.pdh = currentDayHigh_;
    finalized.pdl = currentDayLow_;
    finalized.dayStartUtcMs = *currentDayStartUtcMs_;
    lastFinalizedPdh_ = currentDayHigh_;
    lastFinalizedPdl_ = currentDayLow_;

    currentDayStartUtcMs_ = dayStart;
    currentDayHigh_ = candle.high;
    currentDayLow_ = candle.low;

    return finalized;
}

}  // namespace smc
