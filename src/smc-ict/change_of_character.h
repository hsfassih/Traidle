#pragma once

#include "../candlesticks.h"
#include "smc_types.h"

#include <optional>

using namespace std;

// ---------------------------------------------------------------------------
// change_of_character: reversal confirmation against a *Protected* level,
// as opposed to break_of_structure.cpp's continuation confirmation against
// the current in-trend swing. Mechanically the same trigger (a body close
// beyond a level) but a different anchor and a flipped `bias` on success.
//
// This module does not decide the Protected Low/High itself - that is
// "the swing that fed the current Higher High/Lower Low," which is only
// knowable at the moment a BOS fires (see break_of_structure.h's class
// comment for why). The engine calls setProtectedLow()/setProtectedHigh()
// right after seeing a same-direction BOS; this module just remembers
// whatever it was told and checks against it every candle after that.
//
// It also does not decide the MSS upgrade (displacement + same-direction
// FVG) - see smc_engine.cpp, which is the only place with access to both
// the rolling body average and fair_value_gaps.cpp's output for the same
// candle.
// ---------------------------------------------------------------------------
namespace smc {

class ChochTracker {
public:
    void setProtectedLow(const SwingPoint& swingLow) { protectedLow_ = swingLow; }
    void setProtectedHigh(const SwingPoint& swingHigh) { protectedHigh_ = swingHigh; }

    optional<double> protectedLow() const {
        return protectedLow_ ? optional<double>(protectedLow_->price) : nullopt;
    }
    optional<double> protectedHigh() const {
        return protectedHigh_ ? optional<double>(protectedHigh_->price) : nullopt;
    }

    // Checks the just-closed candle against the currently-relevant
    // protected level (ProtectedLow while bias is Bullish, ProtectedHigh
    // while Bearish) and flips `bias` on a confirmed break. `isMss` on the
    // returned event is always false here - see smc_engine.cpp.
    optional<StructuralEvent> update(const candlesticks::Candlestick& candle, size_t sequence,
                                     Bias& bias);

private:
    optional<SwingPoint> protectedLow_;
    optional<SwingPoint> protectedHigh_;
};

}  // namespace smc
