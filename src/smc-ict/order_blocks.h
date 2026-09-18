#pragma once

#include "../candlesticks.h"
#include "smc_types.h"

#include <deque>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// order_blocks: depends on both fair_value_gaps.cpp and break_of_structure.cpp
// (a candle is only an Order Block once the subsequent leg produces a
// same-direction FVG AND a BOS - see ict_smc_numerical_definitions.md
// section 5), so this module never decides validity on its own; it is fed
// each candle's FVG/BOS results by the engine.
//
// Sub-types actually wired up in this pass: Standard, Breaker (a Standard
// OB whose zone is later fully closed through - not just Mean-Threshold-
// dipped, see below), Propulsion (a new same-bias OB nested inside an
// already-mitigated one), and Rejection (built straight from a swing
// point's wick, independent of FVG/BOS - see updateFromSwing()).
//
// Mitigation Block is intentionally NOT wired to its own detection path in
// this pass - the PDF's own description of it ("similar to a Breaker, but
// originates from a swing that failed to sweep external liquidity before
// reversing... structurally weaker") is thin enough that a confident
// numeric definition isn't there yet; the enum value exists in
// smc_types.h for when it is.
//
// Judgment call worth flagging: the PDF uses "a body close beyond the Mean
// Threshold... invalidates the institutional strength" for invalidation in
// general, but separately describes a Breaker as needing "a massive
// counter-displacement" - a more decisive break than a simple MT dip. This
// module treats those as two severities: `invalidated` fires on an MT
// breach (matches the PDF's literal wording), while the upgrade to
// `OrderBlockSubtype::Breaker` only fires on a full close through the
// zone's FAR edge (the more decisive break the Breaker description
// implies).
// ---------------------------------------------------------------------------
namespace smc {

class OrderBlockTracker {
public:
    struct UpdateResult {
        vector<OrderBlock> formed;
        vector<OrderBlock> changed;  // invalidated / mitigated / breaker-flipped this candle
    };

    // `fvgsFormedThisCandle`/`swingsConfirmedThisCandle`: exactly what
    // fair_value_gaps.cpp / swings.cpp returned from THEIR update() calls
    // for this same candle - the engine passes these straight through.
    UpdateResult update(const candlesticks::Candlestick& candle, size_t sequence,
                       const vector<FairValueGap>& fvgsFormedThisCandle,
                       const vector<SwingPoint>& swingsConfirmedThisCandle,
                       bool bullishBosThisCandle, bool bearishBosThisCandle);

    const deque<OrderBlock>& active() const { return active_; }
    void pruneOlderThan(size_t minSequence);

private:
    static constexpr size_t kLookbackCandles = 64;  // how far back an anchor / qualifying FVG can be
    static constexpr size_t kMaxActive = 256;
    // See the Rejection Block quality-filter comment in order_blocks.cpp.
    static constexpr double kMinRejectionWickFraction = 0.33;

    void checkExistingZones(const candlesticks::Candlestick& candle, UpdateResult& result);
    void formFromAnchor(bool bullish, size_t sequence, const candlesticks::Candlestick& bosCandle,
                        UpdateResult& result);

    struct SequencedCandle {
        candlesticks::Candlestick candle;
        size_t sequence = 0;
    };

    deque<SequencedCandle> recentCandles_;                   // bounded lookback, oldest first
    deque<size_t> recentBullishFvgSequences_;
    deque<size_t> recentBearishFvgSequences_;
    deque<size_t> recentBullishBosSequences_;
    deque<size_t> recentBearishBosSequences_;
    deque<OrderBlock> active_;
};

}  // namespace smc
