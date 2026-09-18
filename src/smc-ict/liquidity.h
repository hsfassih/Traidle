#pragma once

#include "../candlesticks.h"
#include "smc_types.h"
#include "swings.h"

#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// liquidity: Buy-Side/Sell-Side Liquidity (the unswept confirmed swing
// highs/lows already tracked by swings.h - this module doesn't duplicate
// that list, it authoritatively marks them swept) plus Equal Highs/Equal
// Lows.
//
// Unlike break_of_structure.cpp (which only ever checks the SINGLE most
// recent swing for its own BOS-vs-sweep classification), this module scans
// EVERY still-unswept swing on every candle - that is the actual point of
// it: a candle can sweep an old, long-since-superseded swing high at the
// same time break_of_structure.cpp is independently evaluating a totally
// different, more recent one as a BOS (see the 2026-09-03 worked example
// in ict_smc_numerical_definitions.md section 2).
// ---------------------------------------------------------------------------
namespace smc {

class LiquidityTracker {
public:
    struct UpdateResult {
        vector<EqualLevel> newEqualLevels;
        // Every swing this candle swept (ALL still-unswept levels are
        // checked here, unlike break_of_structure.cpp which only ever
        // checks the single most recent one - see the class comment
        // above). Reported as StructuralEvents so the frontend/engine
        // treats a sweep of an old, already-superseded swing the same way
        // it treats any other structural event, rather than only being
        // visible as a mutated flag on the swing point itself.
        vector<StructuralEvent> sweepEvents;
    };

    // `atr`: the current ATR(14) from indicators::IndicatorEngine, already
    // computed for this exact candle/timeframe - reused here as the
    // volatility-scaled tolerance for Equal Highs/Lows (see
    // kEqualLevelAtrMultiplier) instead of inventing a fixed-pip tolerance
    // that doesn't translate to a perpetual future. Mutates `swings`
    // (marks swept swings) - see the class comment above for why this
    // module, not break_of_structure.cpp, owns that.
    UpdateResult update(const candlesticks::Candlestick& candle, size_t sequence,
                       const vector<SwingPoint>& swingsConfirmedThisCandle, optional<double> atr,
                       SwingTracker& swings);

    const deque<EqualLevel>& equalLevels() const { return equalLevels_; }
    void pruneOlderThan(size_t minSequence);

    static constexpr double kEqualLevelAtrMultiplier = 0.1;
    // Fallback tolerance (as a fraction of price) for the rare case ATR
    // isn't available yet (the engine's ATR(14) is still warming up) -
    // see fair_value_gaps.h's displacement multiplier for the same kind of
    // "PDF gives no exact number" situation.
    static constexpr double kEqualLevelFallbackPricePct = 0.0005;

private:
    static constexpr size_t kMaxEqualLevels = 128;

    deque<EqualLevel> equalLevels_;
};

}  // namespace smc
