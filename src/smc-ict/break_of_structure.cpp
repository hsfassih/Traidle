#include "break_of_structure.h"

namespace smc {

vector<StructuralEvent> BosTracker::update(const candlesticks::Candlestick& candle, size_t sequence,
                                           const SwingTracker& swings, Bias& bias) {
    vector<StructuralEvent> events;

    // --- Bullish side: only while bias is Bullish or still Undetermined. ---
    if (bias != Bias::Bearish) {
        if (auto high = swings.mostRecentHigh()) {
            const bool alreadyBroken =
                lastBosHighOpenTime_.has_value() && *lastBosHighOpenTime_ == high->openTime;
            if (!alreadyBroken && candle.close > high->price) {
                StructuralEvent ev;
                ev.kind = StructuralEventKind::BullishBos;
                ev.referencePrice = high->price;
                ev.candleClose = candle.close;
                ev.openTime = candle.openTime;
                ev.sequence = sequence;
                events.push_back(ev);
                bias = Bias::Bullish;
                lastBosHighOpenTime_ = high->openTime;
            }
        }
    }

    // --- Bearish side: only while bias is Bearish or still Undetermined. ---
    if (bias != Bias::Bullish) {
        if (auto low = swings.mostRecentLow()) {
            const bool alreadyBroken =
                lastBosLowOpenTime_.has_value() && *lastBosLowOpenTime_ == low->openTime;
            if (!alreadyBroken && candle.close < low->price) {
                StructuralEvent ev;
                ev.kind = StructuralEventKind::BearishBos;
                ev.referencePrice = low->price;
                ev.candleClose = candle.close;
                ev.openTime = candle.openTime;
                ev.sequence = sequence;
                events.push_back(ev);
                bias = Bias::Bearish;
                lastBosLowOpenTime_ = low->openTime;
            }
        }
    }

    return events;
}

}  // namespace smc
