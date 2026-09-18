#pragma once

#include "../candlesticks.h"
#include "smc_types.h"
#include "swings.h"

#include <optional>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// break_of_structure: trend-continuation confirmation against the single
// most recently confirmed swing point in the trend's direction. A body
// CLOSE beyond that reference is a BOS and moves `bias`; a wick beyond it
// without a body close is left alone here (see liquidity.cpp, which
// independently sweeps EVERY still-unswept swing - including whichever one
// is "most recent" - and is the sole source of LiquiditySweepHigh/Low
// events; duplicating that same wick-only check here would double-report
// the exact same sweep). Bullish BOS is only ever checked while bias is
// Bullish or Undetermined (Undetermined lets the very first genuine break
// in either direction bootstrap the initial bias) - mirrors the bearish
// side.
//
// This module deliberately does NOT decide "Protected Low/High" for
// change_of_character.cpp - it only reports that a BOS fired. The engine
// (smc_engine.cpp) is what, upon seeing a Bullish BOS, asks the
// SwingTracker for its current most-recent Swing Low and hands that to
// change_of_character.cpp as the new Protected Low - see smc_engine.h for
// why that wiring belongs at the orchestration layer rather than here.
// ---------------------------------------------------------------------------
namespace smc {

class BosTracker {
public:
    // Returns 0, 1, or 2 BOS events (in the rare case both a bullish and
    // bearish reference are both broken on the very same candle - only
    // possible while bias is still Undetermined, since afterward only one
    // side is ever checked).
    vector<StructuralEvent> update(const candlesticks::Candlestick& candle, size_t sequence,
                                   const SwingTracker& swings, Bias& bias);

private:
    optional<int64_t> lastBosHighOpenTime_;    // swing-high openTime already used for a bullish BOS
    optional<int64_t> lastBosLowOpenTime_;     // swing-low openTime already used for a bearish BOS
};

}  // namespace smc
