#include "ny_time.h"

#include <ctime>

namespace smc {
namespace {

constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr int64_t kMsPerHour = 60 * kMsPerMinute;
constexpr int64_t kMsPerDay = 24 * kMsPerHour;
constexpr int64_t kEstOffsetMs = -5 * kMsPerHour;
constexpr int64_t kEdtOffsetMs = -4 * kMsPerHour;

// Portable UTC struct-tm -> epoch-seconds, matching the existing
// timegm/_mkgmtime split already used in historical_get.cpp.
time_t toEpochSecondsUtc(tm t) {
#if defined(_WIN32)
    return _mkgmtime(&t);
#else
    return timegm(&t);
#endif
}

tm toUtcTm(int64_t epochMs) {
    const time_t epochSeconds = static_cast<time_t>(epochMs / kMsPerSecond);
    tm result{};
#if defined(_WIN32)
    gmtime_s(&result, &epochSeconds);
#else
    gmtime_r(&epochSeconds, &result);
#endif
    return result;
}

// Epoch ms (UTC) for `day`/`month` (1-indexed) at 00:00:00 UTC of `year`.
int64_t utcMidnightMs(int year, int month, int day) {
    tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    return static_cast<int64_t>(toEpochSecondsUtc(t)) * kMsPerSecond;
}

// Day-of-month (1-indexed) of the Nth Sunday of `month`/`year`, where N=1
// means the first Sunday. `tm_wday` from a UTC struct tm is timezone-free
// (0 = Sunday), which is exactly what's needed here - the calendar
// question "which day is the Nth Sunday of March" doesn't depend on any
// timezone at all.
int nthSundayOfMonth(int year, int month, int n) {
    const int64_t firstOfMonthMs = utcMidnightMs(year, month, 1);
    const tm firstOfMonthTm = toUtcTm(firstOfMonthMs);
    const int daysToFirstSunday = (7 - firstOfMonthTm.tm_wday) % 7;
    const int firstSunday = 1 + daysToFirstSunday;
    return firstSunday + 7 * (n - 1);
}

// The exact UTC instant US clocks spring forward (2nd Sunday of March,
// 02:00 EST = 07:00 UTC, since NY is still on standard time right up to
// this moment).
int64_t springForwardUtcMs(int year) {
    const int day = nthSundayOfMonth(year, /*month=*/3, /*n=*/2);
    return utcMidnightMs(year, 3, day) + 7 * kMsPerHour;
}

// The exact UTC instant US clocks fall back (1st Sunday of November,
// 02:00 EDT = 06:00 UTC, since NY is still on daylight time right up to
// this moment).
int64_t fallBackUtcMs(int year) {
    const int day = nthSundayOfMonth(year, /*month=*/11, /*n=*/1);
    return utcMidnightMs(year, 11, day) + 6 * kMsPerHour;
}

}  // namespace

bool isEasternDaylightTime(int64_t epochMs) {
    const tm t = toUtcTm(epochMs);
    const int year = t.tm_year + 1900;
    const int64_t springForward = springForwardUtcMs(year);
    const int64_t fallBack = fallBackUtcMs(year);
    return epochMs >= springForward && epochMs < fallBack;
}

int64_t newYorkUtcOffsetMs(int64_t epochMs) {
    return isEasternDaylightTime(epochMs) ? kEdtOffsetMs : kEstOffsetMs;
}

int minuteOfNewYorkDay(int64_t epochMs) {
    const int64_t nyLocalMs = epochMs + newYorkUtcOffsetMs(epochMs);
    // Floor-divide so instants before the Unix epoch (not a real concern
    // for market data, but cheap to get right) still land in [0, 1440).
    int64_t minuteOfDay = (nyLocalMs / kMsPerMinute) % 1440;
    if (minuteOfDay < 0) minuteOfDay += 1440;
    return static_cast<int>(minuteOfDay);
}

int64_t newYorkDayStartUtcMs(int64_t epochMs) {
    const int64_t offset = newYorkUtcOffsetMs(epochMs);
    const int64_t nyLocalMs = epochMs + offset;
    int64_t nyLocalDayStartMs = (nyLocalMs / kMsPerDay) * kMsPerDay;
    if (nyLocalMs < 0 && nyLocalMs % kMsPerDay != 0) {
        nyLocalDayStartMs -= kMsPerDay;  // floor, not truncate, toward -inf
    }
    // Convert the NY-local midnight back to a real UTC instant. Note this
    // uses the SAME offset computed for `epochMs`, not a fresh lookup at
    // the day boundary itself - the day boundary and `epochMs` are always
    // within the same local day by construction, so they always share a
    // DST regime (the one calendar day per year with a transition is the
    // one edge case ICT's own literature doesn't resolve any more
    // precisely either, and Binance perpetuals report in UTC regardless).
    return nyLocalDayStartMs - offset;
}

}  // namespace smc
