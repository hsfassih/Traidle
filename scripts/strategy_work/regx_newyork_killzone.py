from reg_seed import R, INTRADAY


REG_EXTRA = [
    R(
        "london_missed_htf_poi",
        "time_rule",
        "The London session fails to tap the intended higher-timeframe PD array objective.",
        "kill_zone == new_york_am AND count(custom_signals.htf_pd_array_tap AND kill_zone == london, ny_minute / tf_minutes + 1) == 0",
        "inferred",
        valid_tfs=INTRADAY,
        notes="Evaluated during the New York AM session from intraday candles since NY midnight. Encodes only the missed-array condition; the separate claim that London did not make the day's extreme stays descriptive.",
    ),
    R(
        "ny_taps_htf_poi",
        "time_rule",
        "The New York session reaches the higher-timeframe PD array, completing the objective London missed.",
        "kill_zone == new_york_am AND custom_signals.htf_pd_array_tap",
        "presenter_stated",
        valid_tfs=INTRADAY,
        notes="Uses the engine's fixed new_york_am kill zone (07:00-10:00 NY) because this lesson does not state a different presenter-specific window.",
    ),
    R(
        "london_taps_htf_poi_and_displaces",
        "time_rule",
        "The London session reaches the higher-timeframe PD array and displaces in the intended trend direction.",
        "kill_zone == new_york_am AND count(custom_signals.first_fvg_after_array_tap AND kill_zone == london, ny_minute / tf_minutes + 1) >= 1",
        "inferred",
        valid_tfs=INTRADAY,
        notes="Evaluated during the New York AM session from intraday candles since NY midnight. Uses the first same-direction FVG after the higher-timeframe tap as the displacement proxy.",
    ),
]
