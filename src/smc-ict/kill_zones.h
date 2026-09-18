#pragma once

#include "smc_types.h"

#include <cstdint>
#include <optional>

using namespace std;

// ---------------------------------------------------------------------------
// kill_zones: a pure time filter, not a price pattern - see
// ict_smc_numerical_definitions.md section 8. Stateless (every function
// here is a pure function of a timestamp), unlike every other concept file,
// so there is no tracker class to feed candles into; the engine just calls
// classify() per candle and windowFor() when building a snapshot for the
// frontend to shade the current view.
//
// None of these windows wrap past NY midnight (Asian is 20:00-00:00, i.e.
// [20:00, 24:00) of ONE calendar day, not a span across two) so no special
// wraparound handling is needed here - see ny_time.h's minuteOfNewYorkDay.
// ---------------------------------------------------------------------------
namespace smc {

// The Kill Zone `openTimeMs` (epoch ms UTC) falls within, if any.
optional<KillZoneName> classifyKillZone(int64_t openTimeMs);

// The [start, end) UTC window, in epoch ms, for `zone` on the NY calendar
// day containing `referenceOpenTimeMs` - what the frontend needs to shade
// that zone as a band on the currently-visible chart range.
KillZoneWindow killZoneWindowFor(KillZoneName zone, int64_t referenceOpenTimeMs);

}  // namespace smc
