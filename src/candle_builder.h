#pragma once

#include "candlesticks.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// candle_builder: pure logic, no networking, no clocks, no threads. Everything
// the Capital.com path needs to turn a stream of bid quotes (and REST history
// bars) into Traidle candles for any of the 15 supported timeframes, kept free
// of I/O so it can be unit-tested exhaustively (tests/candle_builder_test.cpp).
//
// Two ideas hold it together:
//   1. A candle belongs to the UTC bucket its quote's OWN timestamp falls in.
//      Never the local clock - measured on the owner's machine, the PC clock
//      was ~3 s ahead of Capital's servers, which would shift every candle
//      boundary. (Verified: 1m candles built this way from bid quotes matched
//      Capital's REST bid-side history exactly.)
//   2. Intervals Capital has no native resolution for (3m, 2h, 6h, 8h, 12h,
//      3d, 1M) are built the same way live (bucket the ticks) and from the
//      next-smaller native REST resolution in history (aggregate()), so live
//      and backfilled candles are produced by the same bucket math.
// ---------------------------------------------------------------------------
namespace candle_builder {

// A candle is closed on the first quote of a later bucket (event time - as
// fast as Binance's x:true), or, if the feed is quiet, by flush() once the
// SERVER clock has passed bucketEnd + this grace. 1500 ms covers the measured
// WebSocket-to-client latency p95 (~600 ms) plus clock-offset error (~150 ms)
// with room to spare: closing earlier would risk closing a candle a moment
// before its last quote arrives; closing later only delays quiet-market closes.
inline constexpr int64_t kCloseGraceMs = 1500;

// Forex has no real volume. A live candle's baseVolume is its QUOTE COUNT times
// this scale; history uses Capital's lastTradedVolume (also a tick count). 1.0
// until the Phase 7 probe measures the median ratio between the two on the
// owner's feed; set it to that ratio so live and backfilled volumes (which feed
// VWAP and OBV) are on the same scale.
inline constexpr double kLiveVolumeScale = 1.0;

// ---------------------------------------------------------------------------
// UTC calendar helpers (Howard Hinnant's days-from-civil / civil-from-days).
// gmtime/mktime/timegm are thread-unsafe or missing on MinGW; these are pure
// integer arithmetic, so they are thread-safe and identical on Windows and
// Linux. Public because capital_api uses them to parse/format Capital's
// timestamps and the tests use them as a round-trip oracle.
// ---------------------------------------------------------------------------
struct CivilDate {
    int64_t year;
    unsigned month;  // 1..12
    unsigned day;    // 1..31
};

int64_t daysFromCivil(int64_t year, unsigned month, unsigned day);  // days since 1970-01-01
CivilDate civilFromDays(int64_t epochDay);
bool isLeapYear(int64_t year);
unsigned daysInMonth(int64_t year, unsigned month);
int64_t floorDiv(int64_t numerator, int64_t denominator);  // denominator > 0; rounds toward -inf
int64_t floorMod(int64_t numerator, int64_t denominator);  // result in [0, denominator)

// "YYYY-MM-DDTHH:MM:SS" (a space instead of 'T', a ".mmm" fraction and a
// trailing 'Z' are tolerated) -> epoch ms, or nullopt if malformed / not a real
// calendar date. Always interpreted as UTC.
optional<int64_t> parseUtcIso(const string& text);
// Epoch ms -> "YYYY-MM-DDTHH:MM:SS" in UTC (milliseconds are truncated).
string formatUtcIso(int64_t epochMs);

// ---------------------------------------------------------------------------
// Interval math
// ---------------------------------------------------------------------------
struct IntervalSpec {
    string label;  // Binance spelling: "1m".."1M"
    enum class Kind { Fixed, Week, Month } kind = Kind::Fixed;
    int64_t fixedMs = 0;  // bucket length for Kind::Fixed; unused for Week/Month
};

// "1m","3m","5m","15m","30m","1h","2h","4h","6h","8h","12h","1d","3d","1w","1M"
// (case-sensitive: "1m" is one minute, "1M" is one month). nullopt otherwise.
optional<IntervalSpec> intervalFromBinance(const string& binanceInterval);

// UTC-aligned start of the bucket containing tsMs.
int64_t bucketOpen(const IntervalSpec& spec, int64_t tsMs);
// EXCLUSIVE end of the bucket that opens at openMs == the next bucket's open.
// Not `open + fixedMs` for 1w/1M (a month is 28-31 days); closeTime is
// bucketEnd - 1.
int64_t bucketEnd(const IntervalSpec& spec, int64_t openMs);

// Compact candle used for history and aggregation: 6 doubles/ints, 48 bytes,
// so 150,000 of them is ~7 MB.
struct Bar {
    int64_t openMs = 0;
    double o = 0, h = 0, l = 0, c = 0;
    double volume = 0;
};

// Which Capital REST resolution to fetch for an interval, and that
// resolution's bar length (used to size the forming-candle seed request).
struct BaseSource {
    string capitalResolution;  // MINUTE, MINUTE_5, MINUTE_15, MINUTE_30, HOUR, HOUR_4, DAY, WEEK
    int64_t nominalStepMs = 0;
};
BaseSource baseSourceFor(const IntervalSpec& spec);

// How many of the latest base bars are enough to cover the CURRENT bucket of
// `spec`, plus one spare (the newest base bar may not exist yet right at a
// boundary). Capped at 40 (a month from DAY bars is 31 + 1 = 32).
int neededBaseBars(const IntervalSpec& spec);

// Groups ascending base bars by bucketOpen and folds each group: first open,
// last close, max high, min low, SUMMED volume. Never fabricates a bar: a
// bucket with no base bars simply does not appear (weekends stay gaps). The
// caller decides which first/last buckets are complete (selectClosedBuckets).
vector<Bar> aggregate(const vector<Bar>& ascendingBase, const IntervalSpec& spec);

// True when the first aggregated bucket may be missing its earliest base bars:
// the oldest fetched base bar lies after `lowerBoundMs` (so the walk ran out of
// data or hit its cap before reaching the resume point) AND does not sit
// exactly on a bucket boundary. A native-resolution bar is its own bucket, so
// this is never true for identity aggregation.
bool firstBucketMayBePartial(const vector<Bar>& ascendingBase, const IntervalSpec& spec,
                             int64_t lowerBoundMs);

struct SelectOptions {
    int64_t nowMs = 0;            // server "now"
    int64_t safetyMarginMs = 0;   // a bucket counts as closed only if bucketEnd <= now - margin
    bool dropFirstBucket = false; // see firstBucketMayBePartial()
    optional<int64_t> afterOpenMs; // discard buckets with open <= this (already in the CSV)
};
// Applies the completeness rules to aggregate()'s output: drops the first
// bucket if asked, drops every bucket not yet safely closed (this is what
// removes the still-forming last bucket), and drops buckets already stored.
vector<Bar> selectClosedBuckets(const vector<Bar>& aggregated, const IntervalSpec& spec,
                                const SelectOptions& options);

// Turns a Bar into the Candlestick the rest of Traidle consumes (volumes other
// than baseVolume are 0 for Forex).
candlesticks::Candlestick toCandlestick(const Bar& bar, const IntervalSpec& spec,
                                        const string& symbol, bool closed);

// ---------------------------------------------------------------------------
// TickCandleBuilder: one per timeframe worker; single-threaded, owned by that
// worker's io_context thread. Feed it (quote timestamp, bid) pairs.
// ---------------------------------------------------------------------------
class TickCandleBuilder {
public:
    using Candlestick = candlesticks::Candlestick;

    struct Result {
        optional<Candlestick> closed;   // the candle that just closed (closed=true), if any
        optional<Candlestick> forming;  // the current forming candle after this tick, if any
        bool closedPartial = false;     // `closed` was not fully observed: do NOT write/SMC-update it
        bool droppedLate = false;       // tick belonged to an already-closed bucket
    };

    TickCandleBuilder(IntervalSpec spec, string symbol, double volumeScale = kLiveVolumeScale,
                      int64_t closeGraceMs = kCloseGraceMs);

    // Installs the currently forming candle built from REST (see 6.8). It
    // covers its whole bucket, so a candle continued from it is complete.
    // Ticks stamped before asOfMs are ignored afterwards (the seed already
    // contains them). Returns false (and changes nothing) if the bar is
    // invalid or its bucket was already closed by this builder. Replaces any
    // current candle (used again after a reconnect).
    bool seed(const Bar& forming, int64_t asOfMs);

    // REST confirmed the current bucket has no data up to asOfMs (e.g. the
    // market is closed): the first tick of that bucket really is its first.
    void markCovered(int64_t asOfMs);

    // Continuity was lost (reconnect) and a re-seed was not possible: the
    // current candle may be missing ticks, and so may the next one to open.
    void invalidate();

    Result onTick(int64_t tsMs, double bid);

    // If a current candle exists and serverNowMs >= bucketEnd(current) +
    // grace, emits it as closed and clears it. Check lastClosedPartial().
    optional<Candlestick> flush(int64_t serverNowMs);

    // The current forming candle (closed=false) without changing anything.
    optional<Candlestick> peekForming() const;

    bool lastClosedPartial() const { return lastClosedPartial_; }
    bool hasCurrent() const { return current_.has_value(); }
    uint64_t lateDropped() const { return lateDropped_; }
    uint64_t ticksAccepted() const { return ticksAccepted_; }
    const IntervalSpec& spec() const { return spec_; }

private:
    struct Current {
        int64_t openMs = 0;
        double o = 0, h = 0, l = 0, c = 0;
        double volume = 0;
        bool partial = false;
    };

    Candlestick emit(const Current& candle, bool closed) const;
    void openCandle(int64_t bucket, double bid);

    IntervalSpec spec_;
    string symbol_;
    double volumeScale_;
    int64_t closeGraceMs_;

    optional<Current> current_;
    optional<int64_t> lastClosedOpenMs_;  // bucket of the most recent closed candle
    int64_t seedAsOfMs_ = INT64_MIN;
    // Every bucket that opens at or after this time was observed from its very
    // first tick, so a candle built in it is complete. INT64_MAX = nothing
    // known yet (a cold start): the first tick-built candle is partial.
    int64_t coveredFromMs_ = INT64_MAX;
    bool lastClosedPartial_ = false;
    uint64_t lateDropped_ = 0;
    uint64_t ticksAccepted_ = 0;
};

}  // namespace candle_builder