#include "change_of_character.h"

namespace smc {

optional<StructuralEvent> ChochTracker::update(const candlesticks::Candlestick& candle,
                                                size_t sequence, Bias& bias) {
    if (bias == Bias::Bullish && protectedLow_.has_value() && candle.close < protectedLow_->price) {
        StructuralEvent ev;
        ev.kind = StructuralEventKind::BearishChoch;
        ev.referencePrice = protectedLow_->price;
        ev.candleClose = candle.close;
        ev.openTime = candle.openTime;
        ev.sequence = sequence;
        bias = Bias::Bearish;
        protectedLow_.reset();
        return ev;
    }

    if (bias == Bias::Bearish && protectedHigh_.has_value() && candle.close > protectedHigh_->price) {
        StructuralEvent ev;
        ev.kind = StructuralEventKind::BullishChoch;
        ev.referencePrice = protectedHigh_->price;
        ev.candleClose = candle.close;
        ev.openTime = candle.openTime;
        ev.sequence = sequence;
        bias = Bias::Bullish;
        protectedHigh_.reset();
        return ev;
    }

    return nullopt;
}

}  // namespace smc
