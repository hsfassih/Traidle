#include "capital_history.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

using namespace std;
using namespace std::chrono;
using candle_builder::Bar;
using candle_builder::BaseSource;
using candle_builder::IntervalSpec;

namespace capital_history {

namespace {

// How far back "5 years" reaches - parity with Binance's kHistoryWindowMs. It
// only bounds a brand-new CSV; whatever Capital actually has is what we get.
constexpr int64_t kHistoryWindowMs = int64_t(5) * 365 * 24 * 3600 * 1000;

// Hard cap on how many BASE bars one timeframe worker will download for a
// brand-new CSV. 150,000 bars is ~150 requests (30 s at the shared 5 req/s,
// before 15 workers split it) and ~7 MB of Bar vector - enough for ~3.5 months
// of 1-minute history, ~4 years of 30-minute history, and everything for the
// slower timeframes. A 5-year 1-minute backfill (2.6 M bars) would take ~9 minutes
// of the shared request budget and block every other timeframe's startup for
// little charting benefit, so it is deliberately not attempted.
constexpr int64_t kMaxBackfillBaseBars = 150000;

// /prices accepts max=1000 (verified).
constexpr int kPageBars = 1000;

// A range-parameter probe must return at least this many bars to be "usable":
// a server that ignores `max` and returns its default of ~10 bars per page would
// turn a 150-request backfill into 15,000.
constexpr size_t kMinUsablePageBars = 100;

// Calibration sample size and the largest time-zone shift we will believe.
constexpr int kCalibrationSampleBars = 300;
constexpr int64_t kMaxPlausibleShiftMs = int64_t(14) * 3600 * 1000;

// See FormingSeed: overlap rather than gap between the REST seed and live ticks.
constexpr int64_t kSeedOverlapMs = 1500;

// Minimum time between in-place progress updates (status never touches the
// scrolling log - see historical_get.h for why).
constexpr auto kProgressInterval = milliseconds(750);

constexpr int64_t kFiveMinutesMs = 5 * 60 * 1000;
constexpr int64_t kHourMs = 3600 * 1000;

// Capital.com's candle boundaries are assumed to be plain UTC boundaries (a 4-hour
// bar opens at 00:00, 04:00, ...; a daily bar at 00:00; a weekly bar on Monday
// 00:00) because that is what the live tick bucketing and every local aggregation
// use. That is UNVERIFIED for the slower resolutions (a broker may roll its day at
// the 17:00 New York close, i.e. 21:00/22:00 UTC). So instead of trusting it, every
// batch of fetched bars is checked and a misaligned batch is refused: a skipped
// timeframe backfill is a visible log line, silently mis-bucketed candles in a CSV
// are not.
optional<IntervalSpec> nativeSpecFor(const string& resolution) {
    static const map<string, string> labels = {
        {"MINUTE", "1m"},  {"MINUTE_5", "5m"}, {"MINUTE_15", "15m"}, {"MINUTE_30", "30m"},
        {"HOUR", "1h"},    {"HOUR_4", "4h"},   {"DAY", "1d"},        {"WEEK", "1w"}};
    const auto it = labels.find(resolution);
    if (it == labels.end()) {
        return nullopt;
    }
    return candle_builder::intervalFromBinance(it->second);
}

// The first bar that does not open on a UTC boundary of its own resolution, if any.
optional<Bar> firstMisalignedBar(const vector<Bar>& bars, const string& resolution) {
    const auto spec = nativeSpecFor(resolution);
    if (!spec) {
        return nullopt;
    }
    for (const Bar& bar : bars) {
        if (candle_builder::bucketOpen(*spec, bar.openMs) != bar.openMs) {
            return bar;
        }
    }
    return nullopt;
}

const char* const kUnusableLogLine =
    "deep backfill unavailable (to= not understood); using latest 1000 candles only";

}  // namespace

// ---------------------------------------------------------------------------
// Range-parameter calibration
// ---------------------------------------------------------------------------
string formatToParam(const RangeParams& params, int64_t utcMs) {
    return candle_builder::formatUtcIso(utcMs + params.shiftMs);
}

namespace {

// Index of a bar in `bars` whose predecessor opens exactly one 5-minute step
// earlier (so "inclusive" and "exclusive" `to` produce different, recognizable
// last bars), searching from `startIndex` upwards.
optional<size_t> contiguousIndexFrom(const vector<Bar>& bars, size_t startIndex) {
    for (size_t i = max<size_t>(startIndex, 1); i < bars.size(); ++i) {
        if (bars[i].openMs - bars[i - 1].openMs == kFiveMinutesMs) {
            return i;
        }
    }
    return nullopt;
}

struct ProbeResult {
    int status = 0;
    vector<Bar> bars;
};

ProbeResult probeTo(capital::RestClient& rest, const string& epic, const string& to) {
    ProbeResult probe;
    const string target = "/api/v1/prices/" + epic + "?resolution=MINUTE_5&max=1000&to=" + to;
    const capital::HttpResult result = rest.get(target);
    probe.status = result.status;
    if (result.status >= 200 && result.status < 300) {
        probe.bars = capital::parsePriceBars(result.body);
    }
    return probe;
}

}  // namespace

RangeParams calibrateRangeParams(capital::RestClient& rest, const string& epic) {
    RangeParams params;

    // Step 1: a recent sample, using the verified call shape (no `to`).
    const vector<Bar> sample =
        rest.fetchPriceBars(epic, "MINUTE_5", kCalibrationSampleBars, nullopt);
    if (sample.size() < 150) {
        params.note = "too little 5-minute history to calibrate";
        return params;
    }

    // Pick a target well inside the sample - about a third of the way from its
    // OLDEST end - so that a server which interprets `to` in a time zone BEHIND
    // UTC (cutting later than we asked) still has bars to return up to the cut.
    // A recent bar would hit the end of the data instead and look inconsistent.
    const auto targetIndex = contiguousIndexFrom(sample, sample.size() / 3);
    if (!targetIndex) {
        params.note = "no contiguous 5-minute bars to calibrate against";
        return params;
    }
    const int64_t target = sample[*targetIndex].openMs;

    // Step 2: ask for everything up to `target`, spelled in UTC.
    const ProbeResult first = probeTo(rest, epic, candle_builder::formatUtcIso(target));
    if (first.status < 200 || first.status >= 300) {
        params.note = "to= rejected with HTTP " + to_string(first.status);
        return params;
    }
    if (first.bars.size() < kMinUsablePageBars) {
        params.note = "to= returned only " + to_string(first.bars.size()) + " bars (max ignored?)";
        return params;
    }

    // Step 3: where did the page actually end?
    //   last == target                      -> `to` is UTC and inclusive
    //   last == target - 5 min              -> `to` is UTC and exclusive
    //   last differs by a whole number of hours (optionally minus one step)
    //                                       -> `to` is read in another time zone
    // In the shifted case the server treated our "10:00" as 10:00 in a zone z
    // hours ahead of UTC, i.e. it cut at 10:00 - z UTC, so last = target - z
    // (inclusive) or target - z - 5 min (exclusive); z may be negative.
    const int64_t diff = target - first.bars.back().openMs;
    if (diff == 0) {
        params.shiftMs = 0;
        params.toInclusive = true;
    } else if (diff == kFiveMinutesMs) {
        params.shiftMs = 0;
        params.toInclusive = false;
    } else if (candle_builder::floorMod(diff, kHourMs) == 0) {
        params.shiftMs = diff;
        params.toInclusive = true;
    } else if (candle_builder::floorMod(diff - kFiveMinutesMs, kHourMs) == 0) {
        params.shiftMs = diff - kFiveMinutesMs;
        params.toInclusive = false;
    } else {
        params.note = "to= result is inconsistent with any time-zone shift";
        return params;
    }
    if (params.shiftMs > kMaxPlausibleShiftMs || params.shiftMs < -kMaxPlausibleShiftMs) {
        params.shiftMs = 0;
        params.note = "implausible time-zone shift";
        return params;
    }

    // Step 4: verify the conclusion on a second, different bar before trusting it
    // for a 150-request walk.
    const auto verifyIndex = contiguousIndexFrom(sample, (sample.size() * 2) / 3);
    if (!verifyIndex) {
        params.note = "could not verify to= handling";
        params.shiftMs = 0;
        return params;
    }
    const int64_t verifyTarget = sample[*verifyIndex].openMs;
    const ProbeResult second = probeTo(rest, epic, formatToParam(params, verifyTarget));
    const bool secondOk =
        second.status >= 200 && second.status < 300 && second.bars.size() >= kMinUsablePageBars &&
        (second.bars.back().openMs == verifyTarget ||
         second.bars.back().openMs == verifyTarget - kFiveMinutesMs);
    if (!secondOk) {
        params.note = "to= handling failed verification";
        params.shiftMs = 0;
        return params;
    }

    params.usable = true;
    params.note = "to= is UTC-based";
    if (params.shiftMs != 0) {
        params.note = "to= is read with a UTC offset of " + to_string(params.shiftMs / kHourMs) + " h";
    }
    return params;
}

RangeParams sharedRangeParams(capital::RestClient& rest, const string& epic, const LogHandler& log) {
    // One answer per API endpoint (host:port): `to=` is a property of the API, not of
    // an epic or a timeframe. Keyed rather than a single global so a process that talks
    // to more than one endpoint (the offline test runs several mock servers) never
    // reuses one server's answer for another; in production there is exactly one key.
    static mutex cacheMutex;
    static map<string, RangeParams> cached;
    const capital::Endpoints& endpoints = rest.session().endpoints();
    const string key = endpoints.restHost + ":" + endpoints.restPort;

    lock_guard<mutex> lock(cacheMutex);  // later callers wait here for the first caller's answer
    if (const auto found = cached.find(key); found != cached.end()) {
        return found->second;
    }
    try {
        RangeParams measured = calibrateRangeParams(rest, epic);
        cached[key] = measured;
        if (!measured.usable && log) {
            log(kUnusableLogLine);
        }
        return measured;
    } catch (const exception&) {
        // A network hiccup is not an answer about the API: do not cache it, let the
        // next caller try again, and make this caller fall back to one page.
        RangeParams unusable;
        unusable.note = "calibration failed";
        return unusable;
    }
}

// ---------------------------------------------------------------------------
// Forming-candle seed
// ---------------------------------------------------------------------------
FormingSeed fetchFormingSeed(capital::Session& session, capital::RestClient& rest,
                             const string& epic, const IntervalSpec& spec) {
    FormingSeed seed;
    // Sampled BEFORE the request so the REST data (which can only be newer) is
    // guaranteed to cover everything up to asOfMs.
    seed.asOfMs = session.serverNowMs() - kSeedOverlapMs;
    try {
        const BaseSource base = candle_builder::baseSourceFor(spec);
        const vector<Bar> latest = rest.fetchPriceBars(
            epic, base.capitalResolution, candle_builder::neededBaseBars(spec), nullopt);
        if (firstMisalignedBar(latest, base.capitalResolution)) {
            seed.status = FormingSeed::Status::Failed;
            seed.error = "Capital.com's " + base.capitalResolution + " candles are not UTC-aligned";
            return seed;
        }

        // The bucket that was forming at asOfMs. Later buckets are ignored here:
        // their ticks are replayed from the live stream (they start after asOfMs).
        const int64_t bucket = candle_builder::bucketOpen(spec, seed.asOfMs);
        const int64_t end = candle_builder::bucketEnd(spec, bucket);
        vector<Bar> inBucket;
        for (const Bar& bar : latest) {
            if (bar.openMs >= bucket && bar.openMs < end) {
                inBucket.push_back(bar);
            }
        }
        if (inBucket.empty()) {
            seed.status = FormingSeed::Status::EmptyBucket;
            return seed;
        }
        const vector<Bar> folded = candle_builder::aggregate(inBucket, spec);
        seed.bar = folded.front();
        seed.status = FormingSeed::Status::Seeded;
    } catch (const exception& e) {
        seed.status = FormingSeed::Status::Failed;
        seed.error = e.what();
    }
    return seed;
}

// ---------------------------------------------------------------------------
// Backfill
// ---------------------------------------------------------------------------
bool ensureContinuousHistory(capital::Session& session, capital::RestClient& rest,
                             const string& epic, const pipeline::TimeframeTask& task,
                             const filesystem::path& csvPath, const StatusHandler& status,
                             const LogHandler& log, indicators::IndicatorEngine& engine,
                             const historical::HistorySeedHandler& seedHistory,
                             bool replayExisting) {
    try {
        // Restore indicator state and seed the visualizer from whatever is on
        // disk. Always first on the first call - even if nothing needs fetching
        // the engine must be caught up before it is handed to the live path.
        // A repeat catch-up call (replayExisting == false) must NOT replay again:
        // the engine already holds those rows, and feeding them twice would
        // corrupt every rolling window, EMA, VWAP and OBV.
        if (replayExisting) {
            historical::replayCsvIntoEngine(csvPath, engine, seedHistory);
        }

        const auto specOptional = candle_builder::intervalFromBinance(task.binanceInterval);
        if (!specOptional) {
            log("history backfill failed: unsupported interval '" + task.binanceInterval + "'");
            return false;
        }
        const IntervalSpec spec = *specOptional;
        const BaseSource base = candle_builder::baseSourceFor(spec);

        // "Now" is sampled ONCE, before the first request. Buckets are judged
        // closed against this instant: a later "now" could call a bucket closed
        // whose last bars are not in the first page we fetched.
        const int64_t nowMs = session.serverNowMs();

        const auto lastStoredRaw = historical::lastStoredCsvOpenTimeMs(csvPath);
        optional<int64_t> lastStored;
        if (lastStoredRaw) {
            lastStored = candle_builder::bucketOpen(spec, *lastStoredRaw);
        }

        int64_t lowerBound = 0;
        if (lastStored) {
            // The bucket right after the last stored one is the next candle we owe the CSV.
            lowerBound = candle_builder::bucketEnd(spec, *lastStored);
            if (candle_builder::bucketEnd(spec, lowerBound) > nowMs - kClosedSafetyMarginMs) {
                return true;  // that bucket is still forming: already continuous, zero requests
            }
        } else {
            lowerBound = max(nowMs - kHistoryWindowMs, nowMs - kMaxBackfillBaseBars * base.nominalStepMs);
        }

        // Walk BACKWARD with the verified call shape: the latest 1000 base bars
        // first, then (only if the first page does not reach lowerBound and the
        // `to=` parameter has been calibrated) older pages. Each page is the
        // previous 1000 bars that EXIST, so weekends are skipped naturally and no
        // request is ever wasted on a closed market. Collected newest-first.
        vector<Bar> collected;
        collected.reserve(static_cast<size_t>(min<int64_t>(kMaxBackfillBaseBars, 2 * kPageBars)));
        size_t malformed = 0;
        auto lastProgress = steady_clock::now();

        vector<Bar> page = rest.fetchPriceBars(epic, base.capitalResolution, kPageBars, nullopt, &malformed);
        for (;;) {
            // Add this page's bars, newest to oldest, that are older than everything held so far.
            size_t added = 0;
            for (size_t i = page.size(); i-- > 0;) {
                if (collected.empty() || page[i].openMs < collected.back().openMs) {
                    collected.push_back(page[i]);
                    ++added;
                }
            }
            if (added == 0) {
                break;  // empty page, or one with nothing older: history has run out
            }
            const int64_t earliest = collected.back().openMs;
            if (earliest <= lowerBound) {
                break;  // reached the resume point / the window
            }
            if (static_cast<int64_t>(collected.size()) >= kMaxBackfillBaseBars) {
                break;  // memory/request cap
            }

            const RangeParams params = sharedRangeParams(rest, epic, log);
            if (!params.usable) {
                break;  // latest page only
            }
            if (steady_clock::now() - lastProgress > kProgressInterval) {
                status("backfilling history: " + to_string(collected.size()) + " base candle(s) fetched");
                lastProgress = steady_clock::now();
            }
            // One second before the oldest bar we hold: whether `to` is inclusive or
            // exclusive, the page then ends strictly before it.
            page = rest.fetchPriceBars(epic, base.capitalResolution, kPageBars,
                                       formatToParam(params, earliest - 1000), &malformed);
        }

        if (collected.empty()) {
            return true;
        }
        reverse(collected.begin(), collected.end());  // ascending
        if (const auto misaligned = firstMisalignedBar(collected, base.capitalResolution)) {
            log("Capital.com's " + base.capitalResolution + " candles do not open on UTC boundaries (e.g. " +
                candle_builder::formatUtcIso(misaligned->openMs) + " UTC); " + task.rawTimeframe +
                " history was skipped instead of writing misaligned candles");
            return false;
        }

        // Is the oldest aggregated bucket possibly missing its earliest base bars?
        const bool dropFirst = candle_builder::firstBucketMayBePartial(collected, spec, lowerBound);
        if (lastStored && collected.front().openMs > lowerBound + base.nominalStepMs) {
            log("history gap: could not reach back to the last stored candle (" +
                candle_builder::formatUtcIso(*lastStored) + " UTC); the missing range stays empty");
        }

        vector<Bar> buckets = candle_builder::aggregate(collected, spec);
        collected.clear();
        collected.shrink_to_fit();

        candle_builder::SelectOptions options;
        options.nowMs = nowMs;
        options.safetyMarginMs = kClosedSafetyMarginMs;
        options.dropFirstBucket = dropFirst;
        options.afterOpenMs = lastStored;
        const vector<Bar> toWrite = candle_builder::selectClosedBuckets(buckets, spec, options);
        buckets.clear();
        buckets.shrink_to_fit();
        if (malformed > 0) {
            log("skipped " + to_string(malformed) + " malformed price bar(s) from Capital.com");
        }

        if (toWrite.empty()) {
            return true;
        }

        ofstream csv = pipeline::openCsvAppend(csvPath);
        int64_t written = 0;
        for (const Bar& bar : toWrite) {
            const candlesticks::Candlestick candle = candle_builder::toCandlestick(bar, spec, epic, true);
            const auto snapshot = pipeline::writeCsvRow(csv, candle, engine);
            if (seedHistory) {
                seedHistory(candle, snapshot);
            }
            ++written;
            if (steady_clock::now() - lastProgress > kProgressInterval) {
                status("backfilling history: " + to_string(written) + "/" +
                       to_string(toWrite.size()) + " candle(s) written");
                lastProgress = steady_clock::now();
            }
        }

        if (written > 0) {
            log("history backfill complete: added " + to_string(written) + " candle(s)");
        }
        return true;
    } catch (const exception& e) {
        log(string("history backfill failed: ") + e.what());
        return false;
    }
}

}  // namespace capital_history