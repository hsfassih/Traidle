#pragma once

#include "candle_builder.h"
#include "capital_api.h"
#include "historical_get.h"
#include "indicators.h"
#include "pipeline_common.h"

#include <cstdint>
#include <filesystem>
#include <string>

using namespace std;

// ---------------------------------------------------------------------------
// capital_history: the Capital.com analogue of historical_get.cpp. Brings a
// timeframe's CSV up to date from Capital's REST candle history (bid side),
// aggregating locally for the intervals Capital has no native resolution for,
// and builds the REST seed for the first live candle.
//
// It reuses historical_get's own CSV resume-point scan and CSV replay (through
// two thin public wrappers) and pipeline::writeCsvRow(), so a Forex CSV row is
// byte-for-byte the same shape as a Binance one and indicator state is restored
// on restart exactly the same way.
// ---------------------------------------------------------------------------
namespace capital_history {

using StatusHandler = historical::StatusHandler;
using LogHandler = historical::LogHandler;

// A bucket is trusted as closed only if its end is at least this far in the past
// (same name and value as historical_get.cpp's kClosedSafetyMarginMs): absorbs
// clock skew and request latency between sampling "now" and judging a bucket.
// Public because the timeframe worker uses it to detect a bucket that closed
// between the backfill pass and the forming-candle seed.
inline constexpr int64_t kClosedSafetyMarginMs = 2000;

// ---------------------------------------------------------------------------
// The `to=` query parameter of GET /prices is UNVERIFIED (time zone,
// inclusivity, whether `max` still applies). calibrateRangeParams() measures
// it against the live API instead of assuming it: see capital_history.cpp.
// ---------------------------------------------------------------------------
struct RangeParams {
    bool usable = false;       // false => deep backfill is disabled (latest 1000 candles only)
    int64_t shiftMs = 0;       // add to a UTC instant before formatting it into `to=`
    bool toInclusive = true;   // informational: a bar opening exactly at `to` is included
    string note;               // why it is unusable / what was measured (never contains secrets)
};

// Performs the measurement (a few REST calls). Throws on transport failure
// (so a transient network error is not mistaken for "unusable").
RangeParams calibrateRangeParams(capital::RestClient& rest, const string& epic);

// Process-wide cached calibration: the first caller measures, later callers (other
// timeframe workers) wait for and reuse the answer. A transport failure is not
// cached - the next caller retries - and is reported as "unusable" to the
// caller that hit it. Logs the single "deep backfill unavailable" line (via the
// first caller's `log`) when the result is unusable.
RangeParams sharedRangeParams(capital::RestClient& rest, const string& epic, const LogHandler& log);

// "YYYY-MM-DDTHH:MM:SS" for the `to=` parameter, given a UTC instant.
string formatToParam(const RangeParams& params, int64_t utcMs);

// ---------------------------------------------------------------------------
// Forming-candle seed (so the first live candle is not truncated to "ticks since launch")
// ---------------------------------------------------------------------------
struct FormingSeed {
    enum class Status {
        Seeded,       // `bar` holds the forming bucket built from REST
        EmptyBucket,  // REST answered, and the bucket has no data yet (market closed / no quote yet)
        Failed        // REST failed; `error` says why (never contains tokens)
    };
    Status status = Status::Failed;
    candle_builder::Bar bar;
    int64_t asOfMs = 0;  // ticks stamped before this are already contained in `bar`
    string error;
};

// Blocking (REST). Call it from the worker's startup phase or a helper thread,
// never from an io_context handler. asOfMs = serverNow - 1.5 s: deliberately
// OVERLAPPING with the REST data rather than leaving a gap (the only side
// effect of the overlap is a couple of extra tick counts in volume).
FormingSeed fetchFormingSeed(capital::Session& session, capital::RestClient& rest,
                             const string& epic, const candle_builder::IntervalSpec& spec);

// ---------------------------------------------------------------------------
// Backfill. Same contract as historical::ensureContinuousHistory: returns true
// when caught up (including "nothing to do", which costs zero network
// requests); false only on an unrecoverable error already reported via `log`.
// The caller continues with live data either way. Blocking (REST + CSV).
//
// `replayExisting` is true for the first call on an engine (restores indicator
// state from the CSV) and MUST be false for any repeat catch-up call on the same
// engine - see the comment in the .cpp.
// ---------------------------------------------------------------------------
bool ensureContinuousHistory(capital::Session& session, capital::RestClient& rest,
                             const string& epic, const pipeline::TimeframeTask& task,
                             const filesystem::path& csvPath, const StatusHandler& status,
                             const LogHandler& log, indicators::IndicatorEngine& engine,
                             const historical::HistorySeedHandler& seedHistory,
                             bool replayExisting = true);

}  // namespace capital_history