#pragma once

#include "smc_types.h"
#include "swings.h"

#include <optional>

using namespace std;

// ---------------------------------------------------------------------------
// premium_discount: Premium/Discount zones and the three OTE Fibonacci
// levels (61.8% / 70.5% "sweet spot" / 78.6%), derived from the current
// dealing range - the two most recently confirmed swing points of opposite
// type, whichever is chronologically older being the range's origin. Pure
// function of swings.h's state; no independent tracking needed, so this is
// a stateless calculator rather than a class with its own update().
// ---------------------------------------------------------------------------
namespace smc {

class PremiumDiscountCalculator {
public:
    static optional<PremiumDiscountZone> compute(const SwingTracker& swings);

    static constexpr double kOte618 = 0.618;
    static constexpr double kOte705 = 0.705;  // the "sweet spot"
    static constexpr double kOte786 = 0.786;
};

}  // namespace smc
