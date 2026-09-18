#include "order_blocks.h"

#include <algorithm>

namespace smc {
namespace {

bool alreadyHasChanged(const vector<OrderBlock>& changed, int64_t anchorOpenTime) {
    return any_of(changed.begin(), changed.end(),
                  [&](const OrderBlock& ob) { return ob.anchorOpenTime == anchorOpenTime; });
}

}  // namespace

void OrderBlockTracker::checkExistingZones(const candlesticks::Candlestick& candle,
                                           UpdateResult& result) {
    for (auto& ob : active_) {
        if (ob.invalidated) continue;  // fully resolved - stop tracking further changes

        bool changedThisCandle = false;

        if (!ob.mitigated && candle.low <= ob.zoneHigh && candle.high >= ob.zoneLow) {
            ob.mitigated = true;
            ob.mitigatedAtOpenTime = candle.openTime;
            changedThisCandle = true;
        }

        const bool mtBreach =
            ob.bullish ? (candle.close < ob.meanThreshold) : (candle.close > ob.meanThreshold);
        if (mtBreach) {
            ob.invalidated = true;
            ob.invalidatedAtOpenTime = candle.openTime;
            changedThisCandle = true;

            const bool farEdgeBreach =
                ob.bullish ? (candle.close < ob.zoneLow) : (candle.close > ob.zoneHigh);
            if (farEdgeBreach) {
                ob.subtype = OrderBlockSubtype::Breaker;
            }
        }

        if (changedThisCandle && !alreadyHasChanged(result.changed, ob.anchorOpenTime)) {
            result.changed.push_back(ob);
        }
    }
}

void OrderBlockTracker::formFromAnchor(bool bullish, size_t sequence,
                                       const candlesticks::Candlestick& bosCandle,
                                       UpdateResult& result) {
    // Scan backward through recent history (excluding the just-appended
    // current candle) for the most recent candle of the OPPOSITE color -
    // the "last opposing candle prior to the displacement."
    const SequencedCandle* anchor = nullptr;
    for (auto it = recentCandles_.rbegin() + 1; it != recentCandles_.rend(); ++it) {
        const bool isBearish = it->candle.close < it->candle.open;
        const bool isBullish = it->candle.close > it->candle.open;
        if (bullish && isBearish) { anchor = &(*it); break; }
        if (!bullish && isBullish) { anchor = &(*it); break; }
    }
    if (anchor == nullptr) return;

    // The qualifying FVG AND BOS must both have happened AFTER this
    // specific anchor candle - not merely "sometime recently" - otherwise
    // a leftover FVG/BOS from a completely different, already-finished
    // leg (e.g. from just before a bias flip) can spuriously validate an
    // unrelated later anchor. This is what ties the check to "the
    // subsequent sequence of candlesticks" the PDF describes, rather than
    // to a flat time window.
    const auto& fvgSeqs = bullish ? recentBullishFvgSequences_ : recentBearishFvgSequences_;
    const auto& bosSeqs = bullish ? recentBullishBosSequences_ : recentBearishBosSequences_;
    const bool hasFvgAfterAnchor =
        any_of(fvgSeqs.begin(), fvgSeqs.end(), [&](size_t s) { return s > anchor->sequence; });
    const bool hasBosAfterAnchor =
        any_of(bosSeqs.begin(), bosSeqs.end(), [&](size_t s) { return s > anchor->sequence; });
    if (!hasFvgAfterAnchor || !hasBosAfterAnchor) return;

    const bool alreadyUsed = any_of(active_.begin(), active_.end(), [&](const OrderBlock& ob) {
        return ob.anchorOpenTime == anchor->candle.openTime && ob.subtype != OrderBlockSubtype::Rejection;
    });
    if (alreadyUsed) return;

    OrderBlock ob;
    ob.bullish = bullish;
    ob.subtype = OrderBlockSubtype::Standard;
    ob.zoneLow = anchor->candle.low;
    ob.zoneHigh = anchor->candle.high;
    if (bullish) {
        ob.refinedLow = anchor->candle.low;
        ob.refinedHigh = anchor->candle.open;
    } else {
        ob.refinedLow = anchor->candle.open;
        ob.refinedHigh = anchor->candle.high;
    }
    ob.meanThreshold = (ob.zoneHigh + ob.zoneLow) / 2.0;
    ob.anchorOpenTime = anchor->candle.openTime;
    ob.sequence = sequence;

    // Propulsion: does this new OB's zone sit fully inside an
    // already-mitigated OB of the same bias? Only checked against other
    // Standard-lineage OBs (Standard/Breaker/Propulsion itself) - Rejection
    // Blocks are a separate, much more common category (they fire on every
    // confirmed swing) and a coincidental price overlap with one doesn't
    // carry the same "nested continuation" meaning the PDF describes.
    for (const auto& other : active_) {
        if (other.bullish != bullish || !other.mitigated) continue;
        if (other.subtype == OrderBlockSubtype::Rejection) continue;
        if (ob.zoneLow >= other.zoneLow && ob.zoneHigh <= other.zoneHigh) {
            ob.subtype = OrderBlockSubtype::Propulsion;
            break;
        }
    }

    active_.push_back(ob);
    if (active_.size() > kMaxActive) active_.pop_front();
    result.formed.push_back(ob);
    (void)bosCandle;  // kept as a parameter for symmetry/future use (e.g. logging); unused otherwise
}

OrderBlockTracker::UpdateResult OrderBlockTracker::update(
    const candlesticks::Candlestick& candle, size_t sequence,
    const vector<FairValueGap>& fvgsFormedThisCandle,
    const vector<SwingPoint>& swingsConfirmedThisCandle, bool bullishBosThisCandle,
    bool bearishBosThisCandle) {
    UpdateResult result;

    checkExistingZones(candle, result);

    recentCandles_.push_back(SequencedCandle{candle, sequence});
    if (recentCandles_.size() > kLookbackCandles) recentCandles_.pop_front();

    for (const auto& fvg : fvgsFormedThisCandle) {
        (fvg.bullish ? recentBullishFvgSequences_ : recentBearishFvgSequences_).push_back(sequence);
    }
    if (bullishBosThisCandle) recentBullishBosSequences_.push_back(sequence);
    if (bearishBosThisCandle) recentBearishBosSequences_.push_back(sequence);

    const size_t floorSeq = sequence > kLookbackCandles ? sequence - kLookbackCandles : 0;
    while (!recentBullishFvgSequences_.empty() && recentBullishFvgSequences_.front() < floorSeq) {
        recentBullishFvgSequences_.pop_front();
    }
    while (!recentBearishFvgSequences_.empty() && recentBearishFvgSequences_.front() < floorSeq) {
        recentBearishFvgSequences_.pop_front();
    }
    while (!recentBullishBosSequences_.empty() && recentBullishBosSequences_.front() < floorSeq) {
        recentBullishBosSequences_.pop_front();
    }
    while (!recentBearishBosSequences_.empty() && recentBearishBosSequences_.front() < floorSeq) {
        recentBearishBosSequences_.pop_front();
    }

    // --- Rejection Blocks: independent of FVG/BOS, straight from any
    // swing point confirmed this candle. ---
    for (const auto& swing : swingsConfirmedThisCandle) {
        const auto anchorIt =
            find_if(recentCandles_.begin(), recentCandles_.end(),
                    [&](const SequencedCandle& sc) { return sc.candle.openTime == swing.openTime; });
        if (anchorIt == recentCandles_.end()) continue;
        const candlesticks::Candlestick& anchorCandle = anchorIt->candle;

        const bool alreadyUsed = any_of(active_.begin(), active_.end(), [&](const OrderBlock& ob) {
            return ob.subtype == OrderBlockSubtype::Rejection && ob.anchorOpenTime == swing.openTime;
        });
        if (alreadyUsed) continue;

        OrderBlock ob;
        ob.subtype = OrderBlockSubtype::Rejection;
        ob.bullish = !swing.isHigh;  // rejection at a swing LOW => bullish (support); at a HIGH => bearish (resistance)
        if (swing.isHigh) {
            ob.zoneLow = max(anchorCandle.open, anchorCandle.close);
            ob.zoneHigh = anchorCandle.high;
        } else {
            ob.zoneLow = anchorCandle.low;
            ob.zoneHigh = min(anchorCandle.open, anchorCandle.close);
        }

        // Quality filter (not in the PDF as an exact number - a judgment
        // call, same situation as the other tunables in this module): the
        // PDF's own description is "the extreme LONG wick" of a swing -
        // without some minimum, literally every confirmed swing produces a
        // Rejection Block (swings alone already fire on roughly every 2-3
        // candles), which drowns out the cases where the wick is actually
        // the dominant, meaningful feature of the candle. Require the
        // wick to be at least kMinRejectionWickFraction of the candle's
        // total high-low range.
        const double totalRange = anchorCandle.high - anchorCandle.low;
        const double wickSize = ob.zoneHigh - ob.zoneLow;
        if (totalRange <= 0.0 || wickSize / totalRange < kMinRejectionWickFraction) {
            continue;
        }
        ob.refinedLow = ob.zoneLow;
        ob.refinedHigh = ob.zoneHigh;
        ob.meanThreshold = (ob.zoneHigh + ob.zoneLow) / 2.0;
        ob.anchorOpenTime = swing.openTime;
        ob.sequence = sequence;
        active_.push_back(ob);
        if (active_.size() > kMaxActive) active_.pop_front();
        result.formed.push_back(ob);
    }

    // --- Standard Order Blocks (+ Propulsion upgrade). The PDF's own
    // worked ordering can go either way - a BOS can confirm before or
    // after the FVG that validates the same leg - so this triggers on
    // whichever of the two completes the pair: a fresh BOS while a
    // qualifying FVG is already in the window, OR a fresh same-direction
    // FVG while a qualifying BOS is already in the window. Anchor dedup
    // (see formFromAnchor) means re-triggering on both is harmless.
    const bool newBullishFvgThisCandle =
        any_of(fvgsFormedThisCandle.begin(), fvgsFormedThisCandle.end(),
               [](const FairValueGap& f) { return f.bullish; });
    const bool newBearishFvgThisCandle =
        any_of(fvgsFormedThisCandle.begin(), fvgsFormedThisCandle.end(),
               [](const FairValueGap& f) { return !f.bullish; });

    if ((bullishBosThisCandle || newBullishFvgThisCandle) && !recentBullishFvgSequences_.empty() &&
        !recentBullishBosSequences_.empty()) {
        formFromAnchor(/*bullish=*/true, sequence, candle, result);
    }
    if ((bearishBosThisCandle || newBearishFvgThisCandle) && !recentBearishFvgSequences_.empty() &&
        !recentBearishBosSequences_.empty()) {
        formFromAnchor(/*bullish=*/false, sequence, candle, result);
    }

    return result;
}

void OrderBlockTracker::pruneOlderThan(size_t minSequence) {
    while (!active_.empty() && active_.front().sequence < minSequence) {
        active_.pop_front();
    }
}

}  // namespace smc
