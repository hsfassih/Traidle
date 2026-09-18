#include "premium_discount.h"

namespace smc {

optional<PremiumDiscountZone> PremiumDiscountCalculator::compute(const SwingTracker& swings) {
    const auto high = swings.mostRecentHigh();
    const auto low = swings.mostRecentLow();
    if (!high.has_value() || !low.has_value()) return nullopt;

    PremiumDiscountZone zone;
    zone.swingHigh = high->price;
    zone.swingLow = low->price;
    const double delta = zone.swingHigh - zone.swingLow;
    zone.equilibrium = (zone.swingHigh + zone.swingLow) / 2.0;

    // The more recently confirmed of the two is the "terminus" - whichever
    // direction price most recently traveled to reach it. Ties (identical
    // sequence - not possible in practice, since only one swing confirms
    // per candle per type at most, but guarded for safety) default to a
    // bullish leg.
    zone.bullishLeg = high->sequence >= low->sequence;

    if (zone.bullishLeg) {
        zone.ote618 = zone.swingHigh - kOte618 * delta;
        zone.ote705 = zone.swingHigh - kOte705 * delta;
        zone.ote786 = zone.swingHigh - kOte786 * delta;
        zone.originOpenTime = low->openTime;
        zone.terminusOpenTime = high->openTime;
    } else {
        zone.ote618 = zone.swingLow + kOte618 * delta;
        zone.ote705 = zone.swingLow + kOte705 * delta;
        zone.ote786 = zone.swingLow + kOte786 * delta;
        zone.originOpenTime = high->openTime;
        zone.terminusOpenTime = low->openTime;
    }

    return zone;
}

}  // namespace smc
