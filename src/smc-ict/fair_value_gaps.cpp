#include "fair_value_gaps.h"

#include <algorithm>
#include <cmath>

namespace smc {
namespace {

double bodySize(const candlesticks::Candlestick& c) { return abs(c.close - c.open); }

}  // namespace

FvgUpdateResult FvgTracker::update(const candlesticks::Candlestick& candle, size_t sequence,
                                   double rollingAvgBody) {
    FvgUpdateResult result;

    // --- 1. Update every still-open FVG against the candle that just closed. ---
    for (auto& fvg : active_) {
        if (fvg.filled) continue;  // fully resolved - nothing left to track

        bool changedThisCandle = false;

        if (!fvg.mitigated) {
            const double ce = fvg.consequentEncroachment;
            if (candle.low <= ce && candle.high >= ce) {
                fvg.mitigated = true;
                fvg.mitigatedAtOpenTime = candle.openTime;
                changedThisCandle = true;
            }
        }

        const bool farEdgeBroken = fvg.bullish ? (candle.close < fvg.gapLow)
                                               : (candle.close > fvg.gapHigh);
        if (farEdgeBroken) {
            fvg.filled = true;
            fvg.inverted = true;
            fvg.invertedAtOpenTime = candle.openTime;
            if (!fvg.mitigated) {
                fvg.mitigated = true;
                fvg.mitigatedAtOpenTime = candle.openTime;
            }
            changedThisCandle = true;
        }

        if (changedThisCandle) {
            result.changed.push_back(fvg);
        }
    }

    // --- 2. Slide the 3-candle window and test for a newly-confirmed FVG. ---
    window_.push_back(candle);
    if (window_.size() > 3) {
        window_.pop_front();
    }

    if (window_.size() == 3) {
        const auto& first = window_[0];          // C[i-2]
        const auto& displacement = window_[1];    // C[i-1]
        const auto& confirmation = window_[2];    // C[i] == candle

        const bool displacementBig = bodySize(displacement) >= kDisplacementMultiplier * rollingAvgBody;

        FairValueGap fvg;
        bool qualifies = false;

        if (first.high < confirmation.low && displacement.close > displacement.open && displacementBig) {
            fvg.bullish = true;
            fvg.gapLow = first.high;
            fvg.gapHigh = confirmation.low;
            qualifies = true;
        } else if (first.low > confirmation.high && displacement.close < displacement.open && displacementBig) {
            fvg.bullish = false;
            fvg.gapLow = confirmation.high;
            fvg.gapHigh = first.low;
            qualifies = true;
        }

        if (qualifies) {
            fvg.consequentEncroachment = (fvg.gapLow + fvg.gapHigh) / 2.0;
            fvg.formedAtOpenTime = first.openTime;
            fvg.displacementOpenTime = displacement.openTime;
            fvg.confirmedAtOpenTime = confirmation.openTime;
            fvg.sequence = sequence;

            // --- 3. Balanced Price Range: does this new FVG overlap a
            // RECENTLY formed FVG of the opposite direction? ("rapid
            // succession" - see kBprRecencyWindowCandles above.) ---
            const size_t recencyFloor =
                sequence > kBprRecencyWindowCandles ? sequence - kBprRecencyWindowCandles : 0;
            for (const auto& other : active_) {
                if (other.bullish == fvg.bullish) continue;
                if (other.sequence < recencyFloor) continue;
                const double overlapLow = max(other.gapLow, fvg.gapLow);
                const double overlapHigh = min(other.gapHigh, fvg.gapHigh);
                if (overlapLow <= overlapHigh) {
                    BalancedPriceRange bpr;
                    bpr.rangeLow = overlapLow;
                    bpr.rangeHigh = overlapHigh;
                    bpr.formedAtOpenTime = candle.openTime;
                    bpr.sequence = sequence;
                    balancedRanges_.push_back(bpr);
                    result.newBalancedRanges.push_back(bpr);
                }
            }

            active_.push_back(fvg);
            if (active_.size() > kMaxActive) active_.pop_front();
            result.formed.push_back(fvg);
        }
    }

    return result;
}

void FvgTracker::pruneOlderThan(size_t minSequence) {
    while (!active_.empty() && active_.front().sequence < minSequence) {
        active_.pop_front();
    }
    while (!balancedRanges_.empty() && balancedRanges_.front().sequence < minSequence) {
        balancedRanges_.pop_front();
    }
}

void FvgTracker::bulkLoad(deque<FairValueGap> formed, deque<BalancedPriceRange> balancedRanges,
                          deque<candlesticks::Candlestick> tailCandles) {
    active_ = move(formed);
    while (active_.size() > kMaxActive) active_.pop_front();
    balancedRanges_ = move(balancedRanges);
    window_ = move(tailCandles);
    while (window_.size() > 3) window_.pop_front();
}

}  // namespace smc
