from reg_seed import R


REG_EXTRA = [
    R(
        "unicorn_overlap",
        "indicator_relation",
        "A breaker block overlaps a fair value gap on the same timeframe.",
        "custom_signals.breaker_pattern AND close >= open AND max(bullish_fvg[0].gap_low, open) <= min(bullish_fvg[0].gap_high, high) AND NOT bullish_fvg[0].filled",
        "inferred",
        aliases=["unicorn_setup"],
        notes="Canonical bullish side: the breaker block is the current up candle that closes back above the prior swing high; overlap is tested against that candle's range. Distinct from engine order_block.breaker.",
    ),
    R(
        "breaker_mean_threshold",
        "level",
        "50% of the breaker candle body.",
        "(open + close) / 2",
        "presenter_stated",
        aliases=["mean_threshold"],
        notes="Uses the current breaker candle/body, unlike mean_threshold_bodies whose notes assume an order-block candle frozen one bar back.",
    ),
]
