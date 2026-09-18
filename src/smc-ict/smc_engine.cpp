#include "smc_engine.h"

#include <algorithm>
#include <cmath>

namespace smc {
namespace {

double bodySize(const candlesticks::Candlestick& c) { return abs(c.close - c.open); }

}  // namespace

SmcUpdate SmcEngine::update(const candlesticks::Candlestick& candle, optional<double> atr) {
    ++sequence_;
    SmcUpdate result;

    // Captured BEFORE this candle's own body is folded in below - see the
    // lag note at the bottom of this function and fair_value_gaps.cpp's
    // class comment for why the displacement check must never include the
    // very candle it is judging.
    const double avgBodyForThisCandle = rollingBodyAvg_.average();

    auto swingsConfirmed = swings_.update(candle, sequence_);
    auto fvgResult = fvgs_.update(candle, sequence_, avgBodyForThisCandle);

    auto chochEvent = choch_.update(candle, sequence_, bias_);
    auto bosEvents = bos_.update(candle, sequence_, swings_, bias_);

    bool bullishBosThisCandle = false;
    bool bearishBosThisCandle = false;
    for (const auto& ev : bosEvents) {
        result.structuralEvents.push_back(ev);
        if (ev.kind == StructuralEventKind::BullishBos) {
            bullishBosThisCandle = true;
            if (auto low = swings_.mostRecentLow()) choch_.setProtectedLow(*low);
        } else if (ev.kind == StructuralEventKind::BearishBos) {
            bearishBosThisCandle = true;
            if (auto high = swings_.mostRecentHigh()) choch_.setProtectedHigh(*high);
        }
    }

    if (chochEvent.has_value()) {
        // MSS upgrade: this candle is a displacement candle AND leaves
        // behind a same-direction FVG - see change_of_character.h's class
        // comment for why this decision is made here rather than inside
        // ChochTracker itself (it needs both the rolling average this
        // engine already owns and fair_value_gaps.cpp's output for this
        // same candle).
        const bool isBullishChoch = chochEvent->kind == StructuralEventKind::BullishChoch;
        const bool isDisplacement =
            bodySize(candle) >= FvgTracker::kDisplacementMultiplier * avgBodyForThisCandle;
        const bool sameDirectionFvgFormed =
            any_of(fvgResult.formed.begin(), fvgResult.formed.end(),
                  [&](const FairValueGap& f) { return f.bullish == isBullishChoch; });
        chochEvent->isMss = isDisplacement && sameDirectionFvgFormed;
        result.structuralEvents.push_back(*chochEvent);
    }

    auto obResult = obs_.update(candle, sequence_, fvgResult.formed, swingsConfirmed,
                               bullishBosThisCandle, bearishBosThisCandle);
    auto liqResult = liquidity_.update(candle, sequence_, swingsConfirmed, atr, swings_);
    for (const auto& ev : liqResult.sweepEvents) {
        result.structuralEvents.push_back(ev);
    }

    result.swings = move(swingsConfirmed);
    result.fvgsFormed = move(fvgResult.formed);
    result.fvgsChanged = move(fvgResult.changed);
    result.balancedRangesFormed = move(fvgResult.newBalancedRanges);
    result.obsFormed = move(obResult.formed);
    result.obsChanged = move(obResult.changed);
    result.equalLevels = move(liqResult.newEqualLevels);
    result.previousDayLevels = previousLevels_.update(candle);
    result.premiumDiscountZone = PremiumDiscountCalculator::compute(swings_);
    result.killZone = classifyKillZone(candle.openTime);
    result.bias = bias_;

    // Rolling-average lag: fold in the PREVIOUS candle's body now, after
    // it has already been used above, so the value available to the NEXT
    // candle's displacement check still excludes the candle immediately
    // before it (see fair_value_gaps.cpp's class comment - this is what
    // keeps a displacement candle from inflating the very average it is
    // being measured against).
    if (pendingBody_.has_value()) {
        rollingBodyAvg_.push(*pendingBody_);
    }
    pendingBody_ = bodySize(candle);

    // Prune every bounded active-list to the visible-window cap - not on
    // every candle (the scan, while cheap per list, is still needless work
    // on 999 out of every 1000 candles).
    if (sequence_ % kPruneIntervalCandles == 0) {
        const size_t floorSeq = sequence_ > kViewWindowCandles ? sequence_ - kViewWindowCandles : 0;
        fvgs_.pruneOlderThan(floorSeq);
        obs_.pruneOlderThan(floorSeq);
        liquidity_.pruneOlderThan(floorSeq);
    }

    return result;
}

void SmcEngine::replayHistory(const vector<candlesticks::Candlestick>& candles,
                              const vector<optional<double>>& atrPerCandle) {
    // A single ordinary sequential pass - see this class's header comment
    // for the parallelism design reasoning (what was measured, what was
    // prototyped, and why a partial chunked refactor was not shipped under
    // time pressure in favor of keeping this pipeline, now verified
    // against real worked examples, simple and unambiguously correct).
    for (size_t i = 0; i < candles.size(); ++i) {
        const auto atr = i < atrPerCandle.size() ? atrPerCandle[i] : nullopt;
        update(candles[i], atr);
    }
}

SmcSnapshot SmcEngine::currentSnapshot() const {
    SmcSnapshot snapshot;
    snapshot.swingHighs = swings_.recentHighs();
    snapshot.swingLows = swings_.recentLows();
    snapshot.fvgs = fvgs_.active();
    snapshot.balancedRanges = fvgs_.balancedRanges();
    snapshot.orderBlocks = obs_.active();
    snapshot.equalLevels = liquidity_.equalLevels();
    snapshot.pdh = previousLevels_.currentPdh();
    snapshot.pdl = previousLevels_.currentPdl();
    snapshot.premiumDiscountZone = PremiumDiscountCalculator::compute(swings_);
    snapshot.bias = bias_;
    return snapshot;
}

}  // namespace smc
