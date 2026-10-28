from reg_seed import R


REG_EXTRA = [
    R(
        "swing_rates_framework",
        "other",
        "An intermediate-term low and the opposing intermediate-term high are established so the 0, 0.25, 0.5, 0.75 and 1.0 swing rates can be drawn between them.",
        "swing_high[1].open_time > swing_low[1].open_time",
        "inferred",
        aliases=["fibonacci_swing_rates"],
        notes="Canonical bullish side: the origin is the intermediate-term low and the terminus is the intermediate-term high; bearish examples use the mirror table.",
    ),
    R(
        "swing_rate_050",
        "level",
        "The 50% equilibrium swing-rate level between the origin intermediate-term low and the terminus intermediate-term high.",
        "custom_signals.swing_rates_framework",
        "presenter_stated",
        value_expr="custom_signals.intermediate_term_low + 0.5 * (custom_signals.intermediate_term_high - custom_signals.intermediate_term_low)",
        aliases=["swing_rate_equilibrium"],
        notes="Canonical bullish side; bearish examples mirror the anchors.",
    ),
    R(
        "swing_rate_075",
        "level",
        "The 75% swing-rate level between the origin intermediate-term low and the terminus intermediate-term high.",
        "custom_signals.swing_rates_framework",
        "presenter_stated",
        value_expr="custom_signals.intermediate_term_low + 0.75 * (custom_signals.intermediate_term_high - custom_signals.intermediate_term_low)",
        aliases=["silver_bullet_level"],
        notes="Canonical bullish side; this is the shallow-entry quartile nearest the terminus.",
    ),
    R(
        "swing_rate_050_behavior",
        "price_action",
        "Price trades at the 50% swing-rate level and shows the deeper-retracement behavior the presenter expects there: consolidation and/or a liquidity sweep before 0.75 is reached.",
        "(low <= custom_signals.swing_rate_050 AND high >= custom_signals.swing_rate_050) AND (custom_signals.consolidation OR any(structural_event.kind == liquidity_sweep_low, K_REACTION_WINDOW_CANDLES))",
        "inferred",
        aliases=["swing_rate_midpoint_behavior"],
        notes="Uses the exact presenter 0.5 level, while consolidation and the 'immediate' sweep window are proxied with existing registry constants.",
    ),
]


CON_EXTRA = []
