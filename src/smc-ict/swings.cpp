#include "swings.h"

namespace smc {
namespace {

optional<SwingPoint> mostRecentPivot(const deque<SwingPoint>& pts, bool wantHigh) {
    if (pts.size() < 3) return nullopt;
    for (long i = static_cast<long>(pts.size()) - 2; i >= 1; --i) {
        const size_t idx = static_cast<size_t>(i);
        const bool qualifies =
            wantHigh ? (pts[idx].price > pts[idx - 1].price && pts[idx].price > pts[idx + 1].price)
                     : (pts[idx].price < pts[idx - 1].price && pts[idx].price < pts[idx + 1].price);
        if (qualifies) return pts[idx];
    }
    return nullopt;
}

}  // namespace

vector<SwingPoint> SwingTracker::update(const candlesticks::Candlestick& candle, size_t sequence) {
    window_.push_back(candle);
    if (window_.size() > 3) {
        window_.pop_front();
    }

    vector<SwingPoint> confirmed;
    if (window_.size() < 3) {
        return confirmed;
    }

    const auto& left = window_[0];
    const auto& mid = window_[1];
    const auto& right = window_[2];  // == candle, the one that just closed

    if (mid.high > left.high && mid.high > right.high) {
        SwingPoint sp;
        sp.isHigh = true;
        sp.price = mid.high;
        sp.openTime = mid.openTime;
        sp.sequence = sequence;
        highs_.push_back(sp);
        if (highs_.size() > kMaxRetained) highs_.pop_front();
        confirmed.push_back(sp);
    }
    if (mid.low < left.low && mid.low < right.low) {
        SwingPoint sp;
        sp.isHigh = false;
        sp.price = mid.low;
        sp.openTime = mid.openTime;
        sp.sequence = sequence;
        lows_.push_back(sp);
        if (lows_.size() > kMaxRetained) lows_.pop_front();
        confirmed.push_back(sp);
    }
    return confirmed;
}

optional<SwingPoint> SwingTracker::intermediateTermHigh() const { return mostRecentPivot(highs_, true); }
optional<SwingPoint> SwingTracker::intermediateTermLow() const { return mostRecentPivot(lows_, false); }

void SwingTracker::markSwept(bool isHigh, int64_t openTime, int64_t sweptAtOpenTime) {
    auto& list = isHigh ? highs_ : lows_;
    for (auto& sp : list) {
        if (sp.openTime == openTime) {
            sp.swept = true;
            sp.sweptAtOpenTime = sweptAtOpenTime;
            return;
        }
    }
}

void SwingTracker::bulkLoad(deque<SwingPoint> highs, deque<SwingPoint> lows,
                            deque<candlesticks::Candlestick> tailCandles) {
    highs_ = move(highs);
    lows_ = move(lows);
    while (highs_.size() > kMaxRetained) highs_.pop_front();
    while (lows_.size() > kMaxRetained) lows_.pop_front();
    window_ = move(tailCandles);
    while (window_.size() > 3) window_.pop_front();
}

}  // namespace smc
