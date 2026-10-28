from reg_seed import R, INTRADAY


REG_EXTRA = [
    R(
        "prior_session_expansion",
        "session_range",
        "An earlier session in the same New York day already displaced in the trade direction and left at least one fair value gap, so the next session is less likely to fully retrace and sweep it.",
        "(kill_zone == london AND count(bullish_fvg[0].formed AND ny_minute >= 20:00, ny_minute / tf_minutes + 240 / tf_minutes + 1) >= 1) OR (kill_zone == new_york_am AND count(bullish_fvg[0].formed AND (ny_minute >= 20:00 OR ny_minute < 07:00), ny_minute / tf_minutes + 240 / tf_minutes + 1) >= 1)",
        "inferred",
        aliases=[],
        value_expr=None,
        notes="Bullish-side proxy: London checks for an Asian-session displacement FVG; New York checks for an Asian or London displacement FVG before 07:00. 'Strong expansion' is proxied by at least one engine fair value gap in the trade direction.",
        tags=(),
        valid_tfs=INTRADAY,
        reason=None,
    ),
]

CON_EXTRA = []
