#include "liquidity.h"

#include <cmath>

namespace smc {

LiquidityTracker::UpdateResult LiquidityTracker::update(
    const candlesticks::Candlestick& candle, size_t sequence,
    const vector<SwingPoint>& swingsConfirmedThisCandle, optional<double> atr, SwingTracker& swings) {
    UpdateResult result;

    // --- Sweep marking: EVERY still-unswept swing, not just the most recent. ---
    for (const auto& sp : swings.recentHighs()) {
        if (!sp.swept && candle.high > sp.price) {
            swings.markSwept(/*isHigh=*/true, sp.openTime, candle.openTime);
            StructuralEvent ev;
            ev.kind = StructuralEventKind::LiquiditySweepHigh;
            ev.referencePrice = sp.price;
            ev.candleClose = candle.close;
            ev.openTime = candle.openTime;
            ev.sequence = sequence;
            result.sweepEvents.push_back(ev);
        }
    }
    for (const auto& sp : swings.recentLows()) {
        if (!sp.swept && candle.low < sp.price) {
            swings.markSwept(/*isHigh=*/false, sp.openTime, candle.openTime);
            StructuralEvent ev;
            ev.kind = StructuralEventKind::LiquiditySweepLow;
            ev.referencePrice = sp.price;
            ev.candleClose = candle.close;
            ev.openTime = candle.openTime;
            ev.sequence = sequence;
            result.sweepEvents.push_back(ev);
        }
    }

    // --- Equal Highs / Equal Lows: compare each freshly-confirmed swing
    // against the swing immediately before it (same type). ---
    for (const auto& fresh : swingsConfirmedThisCandle) {
        const auto& history = fresh.isHigh ? swings.recentHighs() : swings.recentLows();
        if (history.size() < 2) continue;
        const SwingPoint& previous = history[history.size() - 2];

        const double epsilon = (atr.has_value() && *atr > 0.0)
                                   ? kEqualLevelAtrMultiplier * (*atr)
                                   : kEqualLevelFallbackPricePct * fresh.price;
        if (abs(fresh.price - previous.price) <= epsilon) {
            EqualLevel level;
            level.isHigh = fresh.isHigh;
            level.priceA = previous.price;
            level.priceB = fresh.price;
            level.openTimeA = previous.openTime;
            level.openTimeB = fresh.openTime;
            level.sequence = sequence;
            equalLevels_.push_back(level);
            if (equalLevels_.size() > kMaxEqualLevels) equalLevels_.pop_front();
            result.newEqualLevels.push_back(level);
        }
    }

    return result;
}

void LiquidityTracker::pruneOlderThan(size_t minSequence) {
    while (!equalLevels_.empty() && equalLevels_.front().sequence < minSequence) {
        equalLevels_.pop_front();
    }
}

}  // namespace smc
