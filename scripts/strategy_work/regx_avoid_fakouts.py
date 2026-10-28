from reg_seed import R, INTRADAY

REG_EXTRA = [
    R(
        "opening_judas_swing",
        "price_action",
        "A counter-trend manipulation leg trades below the London session open, below the New York midnight open during the New York AM session, or below the Monday open, then closes back above that opening price.",
        "(kill_zone == london AND ny_minute >= 02:00 AND lowest(low, ((ny_minute - 02:00) / tf_minutes) + 1) < custom_signals.london_open_price AND close > custom_signals.london_open_price) OR (kill_zone == new_york_am AND custom_signals.judas_swing) OR (lowest(low, (((ny_dow - 1) * 1440 + ny_minute) / tf_minutes) + 1) < custom_signals.weekly_open AND close > custom_signals.weekly_open)",
        "inferred",
        notes="Uses the registry's London, daily, and weekly opening-price proxies; the transcript does not give a separate non-equity New York AM opening price beyond the daily open context.",
        valid_tfs=INTRADAY,
    ),
    R(
        "judas_reversal_zone_tap",
        "price_action",
        "An opening Judas swing reaches the first line of defense, usually a bullish fair value gap, or sweeps the last line of defense and closes back above it.",
        "custom_signals.opening_judas_swing AND ((custom_signals.flod AND low <= custom_signals.flod AND NOT bullish_fvg[0].filled) OR (low <= custom_signals.llod AND close >= custom_signals.llod))",
        "inferred",
        notes="The first-line branch uses the presenter's FVG touch semantics, not the engine's CE mitigation requirement; the last-line branch models a sweep/reclaim of the origin swing low.",
        valid_tfs=INTRADAY,
    ),
]

CON_EXTRA = []
