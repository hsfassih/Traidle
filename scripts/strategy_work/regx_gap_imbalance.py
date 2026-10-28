from reg_seed import R


REG_EXTRA = [
    R(
        "fvg_without_displacement",
        "candle_pattern",
        "A three-candle bullish fair value gap with a wick gap between candle 1 and candle 3, regardless of displacement size; mirror for bearish.",
        "high[-2] < low",
        "presenter_stated",
        aliases=["any_size_fvg", "presenter_fvg"],
        notes="Captures the presenter's geometry-only definition. The engine's fvg.bullish additionally requires a displacement candle.",
    ),
]

CON_EXTRA = []
