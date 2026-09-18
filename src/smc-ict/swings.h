#pragma once

#include "../candlesticks.h"
#include "smc_types.h"

#include <deque>
#include <optional>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// swings: the "Three-Candle Rule" - Swing Highs, Swing Lows, and their
// recursive higher-order form (Intermediate-Term High/Low). Everything else
// in smc-ict/ (break_of_structure, change_of_character, liquidity,
// premium_discount) is built on top of this module's output, so it has no
// dependency on any other concept file itself - only candlesticks.h.
//
// Only CLOSED candles are ever fed in (see smc_engine.h) - a swing at
// candle i is only knowable once candle i+1 has also closed, so detection
// is always one candle behind the actual pivot, by construction, never a
// live/forming-candle preview.
// ---------------------------------------------------------------------------
namespace smc {

class SwingTracker {
public:
    // Feed one newly-closed candle, in chronological order. `sequence` is
    // the engine-wide monotonic closed-candle counter for THIS candle (see
    // smc_engine.h) - stamped onto any swing point confirmed by it. Returns
    // 0, 1, or (in the unusual case a single candle is simultaneously a
    // lower-high-flanked high AND a higher-low-flanked low) 2 confirmed
    // swing points.
    vector<SwingPoint> update(const candlesticks::Candlestick& candle, size_t sequence);

    optional<SwingPoint> mostRecentHigh() const {
        return highs_.empty() ? nullopt : optional<SwingPoint>(highs_.back());
    }
    optional<SwingPoint> mostRecentLow() const {
        return lows_.empty() ? nullopt : optional<SwingPoint>(lows_.back());
    }

    // Bounded history of confirmed swings, oldest first - what
    // change_of_character.cpp and premium_discount.cpp read to find
    // "the swing immediately before the current one" and "the current
    // dealing range."
    const deque<SwingPoint>& recentHighs() const { return highs_; }
    const deque<SwingPoint>& recentLows() const { return lows_; }

    // Intermediate-Term High/Low: the same 3-point test applied one level
    // up, to the sequence of already-confirmed Swing Highs/Lows rather than
    // raw candles (PDF: "a Short-Term High flanked by two lower Short-Term
    // Highs"). Returns the most recent one, if any.
    optional<SwingPoint> intermediateTermHigh() const;
    optional<SwingPoint> intermediateTermLow() const;

    // Marks the swing at `openTime` (high or low, whichever matches
    // `isHigh`) as swept as of `sweptAtOpenTime` - called by liquidity.cpp,
    // kept here rather than duplicated since this class already owns the
    // canonical swing list.
    void markSwept(bool isHigh, int64_t openTime, int64_t sweptAtOpenTime);

    // Seeds this tracker directly from precomputed results - used once, at
    // startup, by smc_engine.cpp's parallel bulk pre-scan (see
    // smc_thread_pool.h) instead of replaying every historical candle
    // through update() single-threaded. `tailCandles` must be the actual
    // last <=2 raw candles of the scanned range, so the live path's
    // 3-candle window has correct context to continue seamlessly from
    // here. Replaces any existing state.
    void bulkLoad(deque<SwingPoint> highs, deque<SwingPoint> lows,
                 deque<candlesticks::Candlestick> tailCandles);

private:
    static constexpr size_t kMaxRetained = 128;  // bounded confirmed-swing history, per side

    deque<candlesticks::Candlestick> window_;  // last <=3 closed candles
    deque<SwingPoint> highs_;
    deque<SwingPoint> lows_;
};

}  // namespace smc
