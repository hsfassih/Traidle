#pragma once

#include "../candlesticks.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

using namespace std;

// ---------------------------------------------------------------------------
// smc_types: shared plain data types used across every concept file in
// smc-ict/. This header is infrastructure, not a concept of its own - it
// plays the same role for smc-ict/ that candlesticks.h plays for
// indicators.h and historical_get.h: one dependency-free source of truth
// for the shapes every concept file passes to every other concept file, so
// e.g. order_blocks.cpp and change_of_character.cpp agree on exactly what a
// "swing point" or "bias" is without depending on each other's headers.
//
// Nothing here allocates or computes anything beyond the tiny
// RollingBodyAverage helper at the bottom - it is pure data description,
// the same spirit as candlesticks::Candlestick itself.
//
// `sequence` fields below are a monotonic "how many closed candles has this
// timeframe's SmcEngine ever processed" counter (see smc_engine.h), stamped
// onto every structure at formation time. It is what lets the engine prune
// a structure once it has aged out of the currently-visible window (the
// same 50,000-candle cap visualization.cpp's history ring buffer already
// uses) with a single subtraction - see smc_engine.cpp - without needing to
// retain the candles themselves.
// ---------------------------------------------------------------------------
namespace smc {

// Overall directional read of market structure, maintained by
// break_of_structure.cpp and flipped by change_of_character.cpp. Every
// other concept that cares whether we're "currently bullish or bearish"
// (order_blocks.cpp, premium_discount.cpp) reads this rather than
// re-deriving it.
enum class Bias {
    Undetermined,
    Bullish,
    Bearish,
};

inline const char* biasLabel(Bias bias) {
    switch (bias) {
        case Bias::Bullish: return "bullish";
        case Bias::Bearish: return "bearish";
        case Bias::Undetermined: return "undetermined";
    }
    return "undetermined";
}

// One confirmed 3-candle-rule swing point (swings.cpp).
struct SwingPoint {
    bool isHigh = false;   // true = Swing High, false = Swing Low
    double price = 0.0;    // the high (or low) that qualified
    int64_t openTime = 0;  // anchor candle's open time
    size_t sequence = 0;
    bool swept = false;    // true once a later candle's wick traded through
                            // `price` without a body close beyond it - see
                            // liquidity.cpp
    optional<int64_t> sweptAtOpenTime;
};

// A Fair Value Gap, bullish (BISI) or bearish (SIBI). Bounds are always
// stored low-to-high regardless of type; `bullish` carries the direction.
struct FairValueGap {
    bool bullish = true;
    double gapLow = 0.0;
    double gapHigh = 0.0;
    double consequentEncroachment = 0.0;  // (gapLow + gapHigh) / 2
    int64_t formedAtOpenTime = 0;         // open time of C[i-2] (first candle)
    int64_t displacementOpenTime = 0;     // open time of C[i-1] (displacement candle)
    int64_t confirmedAtOpenTime = 0;      // open time of C[i] (confirmation candle)
    size_t sequence = 0;
    bool mitigated = false;  // price has traded back through the CE
    bool filled = false;     // price has closed through the far edge
    bool inverted = false;   // a later candle's BODY closed through the near
                              // edge - polarity flipped (IFVG)
    optional<int64_t> mitigatedAtOpenTime;
    optional<int64_t> invertedAtOpenTime;
};

// A Balanced Price Range: overlap between one bullish and one bearish FVG.
struct BalancedPriceRange {
    double rangeLow = 0.0;
    double rangeHigh = 0.0;
    int64_t formedAtOpenTime = 0;
    size_t sequence = 0;
};

enum class OrderBlockSubtype {
    Standard,
    Breaker,
    Mitigation,
    Rejection,
    Propulsion,
};

inline const char* orderBlockSubtypeLabel(OrderBlockSubtype subtype) {
    switch (subtype) {
        case OrderBlockSubtype::Standard: return "standard";
        case OrderBlockSubtype::Breaker: return "breaker";
        case OrderBlockSubtype::Mitigation: return "mitigation";
        case OrderBlockSubtype::Rejection: return "rejection";
        case OrderBlockSubtype::Propulsion: return "propulsion";
    }
    return "standard";
}

struct OrderBlock {
    bool bullish = true;
    OrderBlockSubtype subtype = OrderBlockSubtype::Standard;
    double zoneLow = 0.0;    // invalidation-zone low  (= anchor candle low)
    double zoneHigh = 0.0;   // invalidation-zone high (= anchor candle high)
    double refinedLow = 0.0;   // refined execution-zone low
    double refinedHigh = 0.0;  // refined execution-zone high
    double meanThreshold = 0.0;  // (zoneHigh + zoneLow) / 2
    int64_t anchorOpenTime = 0;
    size_t sequence = 0;
    bool invalidated = false;
    bool mitigated = false;  // price has returned into the zone at least once
    optional<int64_t> invalidatedAtOpenTime;
    optional<int64_t> mitigatedAtOpenTime;
};

// One Break of Structure / Change of Character / liquidity-sweep event.
// Immutable once confirmed - so downstream consumers (the frontend) can
// just accumulate these as markers rather than track mutable state.
enum class StructuralEventKind {
    BullishBos,
    BearishBos,
    BullishChoch,
    BearishChoch,
    LiquiditySweepHigh,  // wick-only breach of a swing high (not a BOS)
    LiquiditySweepLow,   // wick-only breach of a swing low
};

inline const char* structuralEventKindLabel(StructuralEventKind kind) {
    switch (kind) {
        case StructuralEventKind::BullishBos: return "bullish_bos";
        case StructuralEventKind::BearishBos: return "bearish_bos";
        case StructuralEventKind::BullishChoch: return "bullish_choch";
        case StructuralEventKind::BearishChoch: return "bearish_choch";
        case StructuralEventKind::LiquiditySweepHigh: return "liquidity_sweep_high";
        case StructuralEventKind::LiquiditySweepLow: return "liquidity_sweep_low";
    }
    return "bullish_bos";
}

struct StructuralEvent {
    StructuralEventKind kind = StructuralEventKind::BullishBos;
    double referencePrice = 0.0;  // the swing/protected level that was broken
    double candleClose = 0.0;     // the breaking candle's own close
    int64_t openTime = 0;
    size_t sequence = 0;
    bool isMss = false;  // CHoCH only: upgraded to a Market Structure Shift?
};

struct EqualLevel {
    bool isHigh = false;  // Equal Highs vs Equal Lows
    double priceA = 0.0;
    double priceB = 0.0;
    int64_t openTimeA = 0;
    int64_t openTimeB = 0;
    size_t sequence = 0;  // sequence of the second (confirming) swing
};

struct PreviousDayLevels {
    optional<double> pdh;
    optional<double> pdl;
    optional<int64_t> dayStartUtcMs;  // NY-midnight boundary this covers, in UTC
};

enum class KillZoneName {
    Asian,
    London,
    NewYorkAm,
    LondonClose,
};

inline const char* killZoneLabel(KillZoneName zone) {
    switch (zone) {
        case KillZoneName::Asian: return "asian";
        case KillZoneName::London: return "london";
        case KillZoneName::NewYorkAm: return "new_york_am";
        case KillZoneName::LondonClose: return "london_close";
    }
    return "asian";
}

struct KillZoneWindow {
    KillZoneName zone = KillZoneName::Asian;
    int64_t startOpenTimeMs = 0;  // inclusive, epoch ms UTC
    int64_t endOpenTimeMs = 0;    // exclusive, epoch ms UTC
};

struct PremiumDiscountZone {
    double swingLow = 0.0;
    double swingHigh = 0.0;
    double equilibrium = 0.0;
    double ote618 = 0.0;
    double ote705 = 0.0;
    double ote786 = 0.0;
    bool bullishLeg = true;  // true: terminus is the swing high (retracing down)
    int64_t originOpenTime = 0;
    int64_t terminusOpenTime = 0;
};

// Rolling average body size (|close - open|) over the last `period` closed
// candles - the shared "is this a displacement candle" building block used
// by both fair_value_gaps.cpp and change_of_character.cpp's CHoCH->MSS
// upgrade. Owned once by smc_engine.cpp (fed one candle at a time, in
// order) and its current average handed to whichever module needs it, so
// the two never disagree about what "the rolling average" currently is.
// Same O(1)-per-push shape as indicators.h's private RollingMeanWindow.
class RollingBodyAverage {
public:
    explicit RollingBodyAverage(size_t period) : period_(period) {}

    void push(double body) {
        values_.push_back(body);
        sum_ += body;
        if (values_.size() > period_) {
            sum_ -= values_.front();
            values_.pop_front();
        }
    }

    bool ready() const { return values_.size() == period_; }

    // Returns the average once warmed up; while warming up, returns the
    // average of whatever has been seen so far rather than nullopt, so
    // callers get a (slightly noisier but available) threshold from the
    // very first candle instead of never flagging any displacement at all
    // during a fresh backfill's first `period` candles.
    double average() const {
        if (values_.empty()) return 0.0;
        return sum_ / static_cast<double>(values_.size());
    }

private:
    size_t period_;
    deque<double> values_;
    double sum_ = 0.0;
};

}  // namespace smc
