#pragma once

#include "../candlesticks.h"
#include "smc_types.h"
#include "swings.h"
#include "fair_value_gaps.h"
#include "break_of_structure.h"
#include "change_of_character.h"
#include "order_blocks.h"
#include "liquidity.h"
#include "previous_levels.h"
#include "premium_discount.h"
#include "kill_zones.h"

#include <deque>
#include <optional>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// smc_engine: one instance per timeframe (owned by that timeframe's worker
// thread in binance.cpp, exactly like indicators::IndicatorEngine), wiring
// together every concept module in dependency order:
//   swings -> {break_of_structure, liquidity, premium_discount}
//   fair_value_gaps -> order_blocks (alongside break_of_structure)
//   break_of_structure -> change_of_character (Protected Low/High handoff)
// See each concept header for why the wiring - which module reads which
// other module's output - lives HERE rather than inside the modules
// themselves: no concept file depends on any other concept file's header,
// only on smc_types.h and (for swings-derived ones) swings.h.
//
// Nothing here is ever written to a CSV or to visualization.cpp's existing
// 50,000-candle history ring buffer - see SmcSnapshot below and
// ict_smc_numerical_definitions.md for why this is a deliberately separate,
// much smaller, in-memory-only store.
//
// --- Parallelism design (read this before changing anything here) ---
// update() (the live, per-candle-close path) is NOT parallelized in any
// way - measured at roughly 1.7us for the full six-module pipeline on
// ordinary hardware, and a closed candle arrives at most once per second
// even on the fastest timeframe, so there is nothing to gain by
// dispatching this to another thread; the dispatch overhead alone would
// exceed the work.
//
// replayHistory() (the backfill/restart replay path, potentially millions
// of candles for a multi-year 1-minute history) is a single, ordinary
// sequential pass - deliberately NOT chunked across threads. A genuinely
// parallel chunked pre-scan was measured and prototyped for the swing/FVG
// FORMATION step specifically (~32% of total per-candle cost, so roughly a
// 25-30% end-to-end speedup was achievable on this box) - swing/FVG
// formation only needs a small local candle window, so it chunks safely.
// It was NOT shipped: correctly consuming those precomputed results in the
// sequential pass without re-paying their cost needs fair_value_gaps.cpp's
// mitigation-checking split out from its formation-detection, which is a
// real refactor, and everything else in this pipeline (BOS bias, CHoCH's
// Protected Low/High, FVG/OB mitigation - a zone opened early can be
// mitigated thousands of candles later) has genuinely open-ended state
// that cannot be safely chunked at all without either a documented
// boundary gap or a full authoritative sequential pass afterward anyway -
// at which point most of the saved time is spent regardless. Given a full
// sequential replay of a worst-case 5-year 1-minute history measures at
// low single digits of seconds on ordinary hardware, and every timeframe
// already replays concurrently with every other timeframe (each gets its
// own OS thread already in binance.cpp), that one-time cost was judged not
// worth the correctness risk of a partial chunked refactor under time
// pressure. The thread pool (smc_thread_pool.h) is still real,
// process-wide, reusable infrastructure - it is used for the one thing
// that is unambiguously safe to move off the calling thread: building a
// SmcSnapshot's serialized wire payload for a newly-connected browser
// (see visualization.cpp, which already offloads the analogous indicator
// history payload to its own dedicated worker thread for exactly the same
// "don't block io_context" reason - this reuses that established pattern
// rather than inventing a second one).
// ---------------------------------------------------------------------------
namespace smc {

constexpr size_t kViewWindowCandles = 50000;  // mirrors visualization.cpp's kHistoryDepth
constexpr size_t kPruneIntervalCandles = 1000;

// Everything that changed as a result of ONE closed candle - the live
// per-tick delta handed to binance.cpp, which forwards it to
// VisualizationServer::pushSmcEvents(). NOT a full snapshot (see
// SmcSnapshot below) - only what's new this tick, so the wire payload for
// an ordinary candle (nothing happened) is empty rather than repeating
// dozens of still-active zones every close.
struct SmcUpdate {
    vector<SwingPoint> swings;
    vector<FairValueGap> fvgsFormed;
    vector<FairValueGap> fvgsChanged;
    vector<BalancedPriceRange> balancedRangesFormed;
    vector<OrderBlock> obsFormed;
    vector<OrderBlock> obsChanged;
    vector<StructuralEvent> structuralEvents;  // BOS / CHoCH / liquidity sweeps
    vector<EqualLevel> equalLevels;
    optional<PreviousDayLevels> previousDayLevels;      // set only on the NY-day rollover candle
    optional<PremiumDiscountZone> premiumDiscountZone;  // current, recomputed whenever swings change
    optional<KillZoneName> killZone;  // the Kill Zone this candle's open time falls in, if any
    Bias bias = Bias::Undetermined;

    bool empty() const {
        return swings.empty() && fvgsFormed.empty() && fvgsChanged.empty() &&
              balancedRangesFormed.empty() && obsFormed.empty() && obsChanged.empty() &&
              structuralEvents.empty() && equalLevels.empty() && !previousDayLevels.has_value();
    }
};

// Everything currently "active"/relevant for one timeframe, rebuilt cheaply
// from each tracker's own bounded active-list accessors - what a browser
// needs once, on connect (see visualization.cpp's setSmcSnapshot()).
struct SmcSnapshot {
    deque<SwingPoint> swingHighs;
    deque<SwingPoint> swingLows;
    deque<FairValueGap> fvgs;
    deque<BalancedPriceRange> balancedRanges;
    deque<OrderBlock> orderBlocks;
    deque<EqualLevel> equalLevels;
    optional<double> pdh;
    optional<double> pdl;
    optional<PremiumDiscountZone> premiumDiscountZone;
    Bias bias = Bias::Undetermined;
};

class SmcEngine {
public:
    // Live path: call once per newly-CLOSED candle, in chronological
    // order - never on a forming/peek candle (every concept file's header
    // repeats this; it is the one rule that must never be violated
    // anywhere in this module). `atr`: that timeframe's current ATR(14)
    // from indicators::IndicatorEngine, reused by liquidity.cpp.
    SmcUpdate update(const candlesticks::Candlestick& candle, optional<double> atr);

    // Backfill/restart path: replays a full ordered history of closed
    // candles (reconstructed from the CSV's own OHLCV columns - see
    // binance.cpp's integration notes for exactly where this is called
    // from) before any update() call. `atrPerCandle` must be the same
    // length as `candles`, index-aligned (the ATR the IndicatorEngine
    // would have reported for that same candle during its own replay).
    void replayHistory(const vector<candlesticks::Candlestick>& candles,
                       const vector<optional<double>>& atrPerCandle);

    SmcSnapshot currentSnapshot() const;
    Bias currentBias() const { return bias_; }

private:
    size_t sequence_ = 0;
    Bias bias_ = Bias::Undetermined;
    RollingBodyAverage rollingBodyAvg_{20};
    optional<double> pendingBody_;

    SwingTracker swings_;
    FvgTracker fvgs_;
    BosTracker bos_;
    ChochTracker choch_;
    OrderBlockTracker obs_;
    LiquidityTracker liquidity_;
    PreviousLevelsTracker previousLevels_;
};

}  // namespace smc
