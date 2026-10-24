#include "candle_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

using namespace std;

namespace candle_builder {

namespace {

constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr int64_t kMsPerHour = 60 * kMsPerMinute;
constexpr int64_t kMsPerDay = 24 * kMsPerHour;
constexpr int64_t kMsPerWeek = 7 * kMsPerDay;

// 1970-01-01 was a Thursday, so 1970-01-05 (epoch day 4) was the first Monday.
// Week buckets start on Monday 00:00 UTC, which is how Capital.com's WEEK
// resolution and Binance's 1w klines both open.
constexpr int64_t kFirstMondayEpochDay = 4;

// Upper bound on how many base bars a forming-candle seed request will ask for
// (a month from DAY bars needs 31 + 1).
constexpr int kMaxSeedBaseBars = 40;

}  // namespace

// ---------------------------------------------------------------------------
// UTC calendar helpers
// ---------------------------------------------------------------------------
int64_t floorDiv(int64_t numerator, int64_t denominator) {
    int64_t quotient = numerator / denominator;
    if ((numerator % denominator != 0) && ((numerator < 0) != (denominator < 0))) {
        --quotient;
    }
    return quotient;
}

int64_t floorMod(int64_t numerator, int64_t denominator) {
    const int64_t remainder = numerator % denominator;
    return remainder < 0 ? remainder + denominator : remainder;
}

// Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms"
// (public domain). Valid for any proleptic-Gregorian date in int64 range.
int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);                  // [0, 399]
    const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;  // [0, 365]
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;   // [0, 146096]
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

CivilDate civilFromDays(int64_t epochDay) {
    epochDay += 719468;
    const int64_t era = (epochDay >= 0 ? epochDay : epochDay - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(epochDay - era * 146097);  // [0, 146096]
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;  // [0, 399]
    const int64_t year = static_cast<int64_t>(yearOfEra) + era * 400;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);  // [0, 365]
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;                                       // [0, 11]
    const unsigned day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;                             // [1, 31]
    const unsigned month = monthPrime < 10 ? monthPrime + 3 : monthPrime - 9;                    // [1, 12]
    return CivilDate{year + (month <= 2), month, day};
}

bool isLeapYear(int64_t year) {
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

unsigned daysInMonth(int64_t year, unsigned month) {
    static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    return (month == 2 && isLeapYear(year)) ? 29u : kDays[month - 1];
}

namespace {

// Parses exactly `count` ASCII digits starting at `pos`.
bool parseDigits(const string& text, size_t pos, size_t count, int64_t& out) {
    if (pos + count > text.size()) {
        return false;
    }
    int64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

optional<int64_t> parseUtcIso(const string& text) {
    int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (text.size() < 19) {
        return nullopt;
    }
    if (!parseDigits(text, 0, 4, year) || text[4] != '-' || !parseDigits(text, 5, 2, month) ||
        text[7] != '-' || !parseDigits(text, 8, 2, day) || (text[10] != 'T' && text[10] != ' ') ||
        !parseDigits(text, 11, 2, hour) || text[13] != ':' || !parseDigits(text, 14, 2, minute) ||
        text[16] != ':' || !parseDigits(text, 17, 2, second)) {
        return nullopt;
    }

    size_t pos = 19;
    int64_t millis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        size_t digits = 0;
        int64_t scale = 100;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) {
                millis += (text[pos] - '0') * scale;
                scale /= 10;
            }
            ++digits;
            ++pos;
        }
        if (digits == 0) {
            return nullopt;
        }
    }
    if (pos < text.size() && text[pos] == 'Z') {
        ++pos;
    }
    if (pos != text.size()) {
        return nullopt;
    }

    if (month < 1 || month > 12 || day < 1 ||
        day > static_cast<int64_t>(daysInMonth(year, static_cast<unsigned>(month))) || hour > 23 ||
        minute > 59 || second > 59) {
        return nullopt;
    }
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * kMsPerDay + hour * kMsPerHour + minute * kMsPerMinute + second * kMsPerSecond +
           millis;
}

string formatUtcIso(int64_t epochMs) {
    const int64_t totalSeconds = floorDiv(epochMs, kMsPerSecond);
    const int64_t days = floorDiv(totalSeconds, 86400);
    const int64_t secondOfDay = floorMod(totalSeconds, 86400);
    const CivilDate date = civilFromDays(days);
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02lld:%02lld:%02lld",
             static_cast<long long>(date.year), date.month, date.day,
             static_cast<long long>(secondOfDay / 3600),
             static_cast<long long>((secondOfDay % 3600) / 60),
             static_cast<long long>(secondOfDay % 60));
    return buffer;
}

// ---------------------------------------------------------------------------
// Interval math
// ---------------------------------------------------------------------------
optional<IntervalSpec> intervalFromBinance(const string& binanceInterval) {
    using Kind = IntervalSpec::Kind;
    struct Entry {
        Kind kind;
        int64_t fixedMs;
    };
    static const unordered_map<string, Entry> kTable = {
        {"1m", {Kind::Fixed, 1 * kMsPerMinute}},
        {"3m", {Kind::Fixed, 3 * kMsPerMinute}},
        {"5m", {Kind::Fixed, 5 * kMsPerMinute}},
        {"15m", {Kind::Fixed, 15 * kMsPerMinute}},
        {"30m", {Kind::Fixed, 30 * kMsPerMinute}},
        {"1h", {Kind::Fixed, 1 * kMsPerHour}},
        {"2h", {Kind::Fixed, 2 * kMsPerHour}},
        {"4h", {Kind::Fixed, 4 * kMsPerHour}},
        {"6h", {Kind::Fixed, 6 * kMsPerHour}},
        {"8h", {Kind::Fixed, 8 * kMsPerHour}},
        {"12h", {Kind::Fixed, 12 * kMsPerHour}},
        {"1d", {Kind::Fixed, 1 * kMsPerDay}},
        // ASSUMPTION (Binance's convention, not independently verified for
        // Forex): 3d buckets are multiples of 3 epoch-days, i.e. they start on
        // days where (days since 1970-01-01) % 3 == 0. As a fixed 72 h bucket
        // that is exactly floor(ts / 72h) * 72h.
        {"3d", {Kind::Fixed, 3 * kMsPerDay}},
        {"1w", {Kind::Week, 0}},
        {"1M", {Kind::Month, 0}},
    };
    const auto it = kTable.find(binanceInterval);
    if (it == kTable.end()) {
        return nullopt;
    }
    IntervalSpec spec;
    spec.label = binanceInterval;
    spec.kind = it->second.kind;
    spec.fixedMs = it->second.fixedMs;
    return spec;
}

int64_t bucketOpen(const IntervalSpec& spec, int64_t tsMs) {
    switch (spec.kind) {
        case IntervalSpec::Kind::Fixed:
            return floorDiv(tsMs, spec.fixedMs) * spec.fixedMs;
        case IntervalSpec::Kind::Week: {
            const int64_t day = floorDiv(tsMs, kMsPerDay);
            const int64_t weekStartDay = day - floorMod(day - kFirstMondayEpochDay, 7);
            return weekStartDay * kMsPerDay;
        }
        case IntervalSpec::Kind::Month: {
            const CivilDate date = civilFromDays(floorDiv(tsMs, kMsPerDay));
            return daysFromCivil(date.year, date.month, 1) * kMsPerDay;
        }
    }
    return tsMs;
}

int64_t bucketEnd(const IntervalSpec& spec, int64_t openMs) {
    switch (spec.kind) {
        case IntervalSpec::Kind::Fixed:
            return openMs + spec.fixedMs;
        case IntervalSpec::Kind::Week:
            return openMs + kMsPerWeek;
        case IntervalSpec::Kind::Month: {
            const CivilDate date = civilFromDays(floorDiv(openMs, kMsPerDay));
            const int64_t nextYear = date.month == 12 ? date.year + 1 : date.year;
            const unsigned nextMonth = date.month == 12 ? 1u : date.month + 1;
            return daysFromCivil(nextYear, nextMonth, 1) * kMsPerDay;
        }
    }
    return openMs;
}

BaseSource baseSourceFor(const IntervalSpec& spec) {
    // Capital.com's native REST resolutions are MINUTE, MINUTE_5, MINUTE_15,
    // MINUTE_30, HOUR, HOUR_4, DAY and WEEK. Everything else is aggregated
    // from the largest native resolution that divides it evenly and whose
    // bucket boundaries line up with ours (HOUR_4 opens at 00/04/08/12/16/20
    // UTC, so 8h and 12h aggregate from it; 6h would NOT line up with the
    // 4-hourly grid, so it uses HOUR).
    static const unordered_map<string, BaseSource> kTable = {
        {"1m", {"MINUTE", kMsPerMinute}},
        {"3m", {"MINUTE", kMsPerMinute}},
        {"5m", {"MINUTE_5", 5 * kMsPerMinute}},
        {"15m", {"MINUTE_15", 15 * kMsPerMinute}},
        {"30m", {"MINUTE_30", 30 * kMsPerMinute}},
        {"1h", {"HOUR", kMsPerHour}},
        {"2h", {"HOUR", kMsPerHour}},
        {"4h", {"HOUR_4", 4 * kMsPerHour}},
        {"6h", {"HOUR", kMsPerHour}},
        {"8h", {"HOUR_4", 4 * kMsPerHour}},
        {"12h", {"HOUR_4", 4 * kMsPerHour}},
        {"1d", {"DAY", kMsPerDay}},
        {"3d", {"DAY", kMsPerDay}},
        {"1w", {"WEEK", kMsPerWeek}},
        {"1M", {"DAY", kMsPerDay}},
    };
    const auto it = kTable.find(spec.label);
    if (it == kTable.end()) {
        return BaseSource{"MINUTE", kMsPerMinute};
    }
    return it->second;
}

int neededBaseBars(const IntervalSpec& spec) {
    const BaseSource base = baseSourceFor(spec);
    int64_t longestBucketMs = spec.fixedMs;
    if (spec.kind == IntervalSpec::Kind::Week) {
        longestBucketMs = kMsPerWeek;
    } else if (spec.kind == IntervalSpec::Kind::Month) {
        longestBucketMs = 31 * kMsPerDay;
    }
    const int64_t covering = (longestBucketMs + base.nominalStepMs - 1) / base.nominalStepMs;
    return static_cast<int>(min<int64_t>(covering + 1, kMaxSeedBaseBars));
}

vector<Bar> aggregate(const vector<Bar>& ascendingBase, const IntervalSpec& spec) {
    vector<Bar> out;
    out.reserve(ascendingBase.size());
    for (const Bar& bar : ascendingBase) {
        const int64_t open = bucketOpen(spec, bar.openMs);
        if (!out.empty() && out.back().openMs == open) {
            Bar& folded = out.back();
            folded.h = max(folded.h, bar.h);
            folded.l = min(folded.l, bar.l);
            folded.c = bar.c;
            folded.volume += bar.volume;
        } else {
            Bar fresh = bar;
            fresh.openMs = open;
            out.push_back(fresh);
        }
    }
    return out;
}

bool firstBucketMayBePartial(const vector<Bar>& ascendingBase, const IntervalSpec& spec,
                             int64_t lowerBoundMs) {
    if (ascendingBase.empty()) {
        return false;
    }
    const int64_t earliest = ascendingBase.front().openMs;
    if (earliest <= lowerBoundMs) {
        return false;  // the walk went back at least as far as the resume point
    }
    return bucketOpen(spec, earliest) != earliest;
}

vector<Bar> selectClosedBuckets(const vector<Bar>& aggregated, const IntervalSpec& spec,
                                const SelectOptions& options) {
    vector<Bar> out;
    out.reserve(aggregated.size());
    const int64_t closedBefore = options.nowMs - options.safetyMarginMs;
    for (size_t i = 0; i < aggregated.size(); ++i) {
        const Bar& bar = aggregated[i];
        if (i == 0 && options.dropFirstBucket) {
            continue;
        }
        if (options.afterOpenMs && bar.openMs <= *options.afterOpenMs) {
            continue;
        }
        if (bucketEnd(spec, bar.openMs) > closedBefore) {
            continue;  // still forming (or too fresh to trust): the live path owns it
        }
        out.push_back(bar);
    }
    return out;
}

candlesticks::Candlestick toCandlestick(const Bar& bar, const IntervalSpec& spec,
                                        const string& symbol, bool closed) {
    candlesticks::Candlestick candle{};
    candle.symbol = symbol;
    candle.openTime = bar.openMs;
    candle.closeTime = bucketEnd(spec, bar.openMs) - 1;
    candle.open = bar.o;
    candle.high = bar.h;
    candle.low = bar.l;
    candle.close = bar.c;
    candle.baseVolume = bar.volume;
    candle.quoteVolume = 0;
    candle.takerBuyBaseVolume = 0;
    candle.takerBuyQuoteVolume = 0;
    candle.closed = closed;
    return candle;
}

// ---------------------------------------------------------------------------
// TickCandleBuilder
// ---------------------------------------------------------------------------
TickCandleBuilder::TickCandleBuilder(IntervalSpec spec, string symbol, double volumeScale,
                                     int64_t closeGraceMs)
    : spec_(move(spec)),
      symbol_(move(symbol)),
      volumeScale_(volumeScale),
      closeGraceMs_(closeGraceMs) {}

TickCandleBuilder::Candlestick TickCandleBuilder::emit(const Current& candle, bool closed) const {
    Candlestick out{};
    // `symbol_` is an epic such as "EURUSD": short enough for the small-string
    // optimization, so building a Candlestick per tick does not allocate.
    out.symbol = symbol_;
    out.openTime = candle.openMs;
    out.closeTime = bucketEnd(spec_, candle.openMs) - 1;
    out.open = candle.o;
    out.high = candle.h;
    out.low = candle.l;
    out.close = candle.c;
    out.baseVolume = candle.volume;
    out.quoteVolume = 0;
    out.takerBuyBaseVolume = 0;
    out.takerBuyQuoteVolume = 0;
    out.closed = closed;
    return out;
}

void TickCandleBuilder::openCandle(int64_t bucket, double bid) {
    Current fresh;
    fresh.openMs = bucket;
    fresh.o = fresh.h = fresh.l = fresh.c = bid;
    fresh.volume = volumeScale_;
    // Partial unless we were provably listening before this bucket began.
    fresh.partial = bucket < coveredFromMs_;
    // From here on we have been receiving continuously, so every bucket that
    // starts after this one ends will be seen from its first tick.
    coveredFromMs_ = min(coveredFromMs_, bucketEnd(spec_, bucket));
    current_ = fresh;
}

bool TickCandleBuilder::seed(const Bar& forming, int64_t asOfMs) {
    // A bucket this builder has already closed (e.g. by the flush timer while a
    // slow REST re-seed was in flight) must never be re-opened: it would be
    // emitted, and so written to the CSV, a second time.
    if (lastClosedOpenMs_ && bucketOpen(spec_, forming.openMs) <= *lastClosedOpenMs_) {
        return false;
    }
    const bool valid = isfinite(forming.o) && isfinite(forming.h) && isfinite(forming.l) &&
                       isfinite(forming.c) && forming.o > 0 && forming.h > 0 && forming.l > 0 &&
                       forming.c > 0 && isfinite(forming.volume);
    if (!valid) {
        return false;
    }
    Current seeded;
    seeded.openMs = bucketOpen(spec_, forming.openMs);
    seeded.o = forming.o;
    // Defensive: keep the OHLC invariant even if a REST bar was slightly off.
    seeded.h = max({forming.h, forming.o, forming.c});
    seeded.l = min({forming.l, forming.o, forming.c});
    seeded.c = forming.c;
    seeded.volume = forming.volume;
    seeded.partial = false;  // REST covers the candle back to its bucket start
    current_ = seeded;
    seedAsOfMs_ = asOfMs;
    coveredFromMs_ = min(coveredFromMs_, seeded.openMs);
    return true;
}

void TickCandleBuilder::markCovered(int64_t asOfMs) {
    coveredFromMs_ = min(coveredFromMs_, bucketOpen(spec_, asOfMs));
}

void TickCandleBuilder::invalidate() {
    if (current_) {
        current_->partial = true;
    }
    coveredFromMs_ = INT64_MAX;
}

TickCandleBuilder::Result TickCandleBuilder::onTick(int64_t tsMs, double bid) {
    Result result;
    if (!isfinite(bid) || !(bid > 0.0)) {
        return result;
    }
    if (tsMs < seedAsOfMs_) {
        return result;  // already contained in the REST seed
    }

    const int64_t bucket = bucketOpen(spec_, tsMs);

    if (current_) {
        if (bucket == current_->openMs) {
            Current& candle = *current_;
            if (bid > candle.h) candle.h = bid;
            if (bid < candle.l) candle.l = bid;
            candle.c = bid;
            candle.volume += volumeScale_;
            ++ticksAccepted_;
            result.forming = emit(candle, false);
            return result;
        }
        if (bucket < current_->openMs) {
            ++lateDropped_;
            result.droppedLate = true;
            return result;
        }
        // Rollover: this quote belongs to a later bucket, so the current candle
        // is final. Closing on the first quote of the next bucket is as fast
        // as the exchange's own "closed" flag on Binance.
        result.closed = emit(*current_, true);
        result.closedPartial = current_->partial;
        lastClosedPartial_ = current_->partial;
        lastClosedOpenMs_ = current_->openMs;
        current_.reset();
    } else if (lastClosedOpenMs_ && bucket <= *lastClosedOpenMs_) {
        // The timer (flush) already closed this bucket; a straggler must not
        // reopen it or we would emit the same candle twice.
        ++lateDropped_;
        result.droppedLate = true;
        return result;
    }

    openCandle(bucket, bid);
    ++ticksAccepted_;
    result.forming = emit(*current_, false);
    return result;
}

optional<TickCandleBuilder::Candlestick> TickCandleBuilder::peekForming() const {
    if (!current_) {
        return nullopt;
    }
    return emit(*current_, false);
}

optional<TickCandleBuilder::Candlestick> TickCandleBuilder::flush(int64_t serverNowMs) {
    if (!current_) {
        return nullopt;
    }
    if (serverNowMs < bucketEnd(spec_, current_->openMs) + closeGraceMs_) {
        return nullopt;
    }
    Candlestick closed = emit(*current_, true);
    lastClosedPartial_ = current_->partial;
    lastClosedOpenMs_ = current_->openMs;
    current_.reset();
    return closed;
}

}  // namespace candle_builder