#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// ny_time: a small, dependency-free, DST-aware UTC -> America/New_York
// converter. Exists because two smc-ict concepts - previous_levels.cpp
// (PDH/PDL, session ranges) and kill_zones.cpp - are defined in ICT source
// material against New York local time, which is a NEW notion of "day" in
// this codebase, deliberately distinct from indicators.h's existing
// UTC-midnight VWAP reset (see ict_smc_numerical_definitions.md, section 7,
// for why keeping those two "day" boundaries separate is intentional, not a
// bug to reconcile).
//
// US daylight saving rules have been fixed by federal law since 2007
// (Energy Policy Act of 2005): clocks spring forward on the second Sunday
// of March at 02:00 local standard time, and fall back on the first Sunday
// of November at 02:00 local daylight time. That is a small, exact
// computation for this one specific timezone - no tzdata/ICU dependency
// needed, and nothing here reads the host machine's local timezone at all
// (deliberately: this must give the same answer on the Windows/MinGW build
// machine regardless of what timezone that machine is set to).
// ---------------------------------------------------------------------------
namespace smc {

// True if `epochMs` (UTC) falls within US Eastern Daylight Time.
bool isEasternDaylightTime(int64_t epochMs);

// UTC offset for New York at this instant, in milliseconds: either
// -14400000 (-4h, EDT) or -18000000 (-5h, EST).
int64_t newYorkUtcOffsetMs(int64_t epochMs);

// The NY-local minute-of-day (0-1439) for `epochMs` - what kill_zones.cpp
// checks each closed candle's open time against.
int minuteOfNewYorkDay(int64_t epochMs);

// Start of the NY calendar day (NY midnight, expressed back as a real UTC
// epoch ms) that contains `epochMs` - the bucket previous_levels.cpp groups
// candles by for PDH/PDL.
int64_t newYorkDayStartUtcMs(int64_t epochMs);

}  // namespace smc
