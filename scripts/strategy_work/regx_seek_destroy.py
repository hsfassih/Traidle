from reg_seed import R


REG_EXTRA = [
    R(
        "equilibrium_reversion",
        "price_action",
        "Price trades back to the 50% midpoint of the current dual-sweep dealing range.",
        "custom_signals.dealing_range AND low <= (swing_low[0].price + swing_high[0].price) / 2 AND high >= (swing_low[0].price + swing_high[0].price) / 2",
        "inferred",
        aliases=["equilibrium_return"],
        notes="Uses the current dealing-range swings as the Fibonacci anchors because the engine does not store a hand-drawn expansion leg.",
    ),
    R(
        "pd_array_failure",
        "other",
        "After price returns to equilibrium, premium or discount arrays stop producing follow-through fair value gaps and price behaves as consolidation.",
        "any(custom_signals.equilibrium_reversion, K_REACTION_WINDOW_CANDLES) AND custom_signals.consolidation AND count(fvg.formed, K_REACTION_WINDOW_CANDLES) == 0",
        "inferred",
        aliases=["pd_arrays_not_following_through"],
        notes="Wick-heavy candles and 'no follow-through' are proxied by consolidation plus the absence of any new fair value gap inside the reaction window.",
    ),
    R(
        "htf_range_displacement",
        "price_action",
        "A higher-timeframe bullish displacement candle forms a fair value gap completely above the prior consolidation range, ending Seek and Destroy conditions.",
        "any(custom_signals.pd_array_failure, K_CONSOLIDATION_CANDLES) AND custom_signals.displacement_candle AND bullish_fvg[0].formed AND bullish_fvg[0].gap_low > highest(high[-1], K_CONSOLIDATION_CANDLES)",
        "inferred",
        aliases=["range_displacement_exit"],
        notes="Canonical bullish side: the new FVG must print entirely above the previous consolidation window; the bearish side is the mirror.",
    ),
]

CON_EXTRA = []
