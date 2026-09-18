#pragma once

#include "../candlesticks.h"
#include "smc_types.h"

#include <deque>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// fair_value_gaps: Bullish (BISI) / Bearish (SIBI) FVGs, their Consequent
// Encroachment midpoint, mitigation/fill tracking, Inversion FVGs (IFVG),
// and Balanced Price Ranges (BPR). A pure 3-candle test - no dependency on
// swings/BOS, so it can run independently of (and in parallel with, during
// bulk backfill - see smc_thread_pool.h) the swing/structure pipeline.
//
// "Filled" and "inverted" fire together, at the same threshold: once a
// later candle's BODY closes all the way through the gap's far edge, the
// imbalance is both fully traded through (filled) and has flipped polarity
// (inverted) in the same instant - see ict_smc_numerical_definitions.md
// section 4 for the reasoning.
//
// Nothing is ever deleted from `active()` once formed; entries just get
// flagged mitigated/filled/inverted so the frontend can render a resolved
// zone differently from a live one. Age-based pruning (see pruneOlderThan)
// is the only thing that actually removes an entry, keeping this list
// consistent with "currently in the visible window."
// ---------------------------------------------------------------------------
namespace smc {

struct FvgUpdateResult {
    vector<FairValueGap> formed;                  // newly confirmed by this candle
    vector<FairValueGap> changed;                  // existing FVGs whose status changed
    vector<BalancedPriceRange> newBalancedRanges;
};

class FvgTracker {
public:
    // `rollingAvgBody`: the current rolling average body size (see
    // RollingBodyAverage in smc_types.h) - owned once by smc_engine.cpp and
    // passed in here, so this module and change_of_character.cpp's
    // MSS-upgrade check always agree on what "the average" currently is.
    FvgUpdateResult update(const candlesticks::Candlestick& candle, size_t sequence,
                           double rollingAvgBody);

    const deque<FairValueGap>& active() const { return active_; }
    const deque<BalancedPriceRange>& balancedRanges() const { return balancedRanges_; }

    // Drops entries formed before `minSequence` - called once per candle by
    // smc_engine.cpp with `minSequence = currentSequence - kViewWindowCandles`
    // so state never grows past what's relevant to the visible window.
    void pruneOlderThan(size_t minSequence);

    static constexpr double kDisplacementMultiplier = 1.5;

    // Seeds this tracker directly from precomputed results - used once, at
    // startup, by smc_engine.cpp's parallel bulk pre-scan (see
    // smc_thread_pool.h). `tailCandles` must be the actual last <=2 raw
    // candles of the scanned range for the live path's window to continue
    // correctly from here. Replaces any existing state; mitigation/fill/
    // inversion flags on `formed` are trusted as given (the caller is
    // responsible for having already applied them via a sequential pass -
    // see smc_engine.cpp).
    void bulkLoad(deque<FairValueGap> formed, deque<BalancedPriceRange> balancedRanges,
                 deque<candlesticks::Candlestick> tailCandles);

    // The PDF requires a BPR's two opposite-direction FVGs to overlap "in
    // rapid succession" / "immediately" - it gives no exact number (same
    // situation as the displacement multiplier above), so this is a
    // proposed default: only opposite-type FVGs formed within the last
    // `kBprRecencyWindowCandles` closed candles are considered for a BPR
    // match, not the entire multi-year active list (an unconstrained
    // any-time overlap check produces hundreds of BPR matches across a
    // 5-year daily history purely from ordinary price oscillation, which
    // does not match the PDF's "aggressive displacement immediately met
    // with a violent counter-displacement" framing).
    static constexpr size_t kBprRecencyWindowCandles = 10;

private:
    static constexpr size_t kMaxActive = 512;

    deque<candlesticks::Candlestick> window_;  // last <=3 closed candles
    deque<FairValueGap> active_;               // most recent last
    deque<BalancedPriceRange> balancedRanges_;
};

}  // namespace smc
