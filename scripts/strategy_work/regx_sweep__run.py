from reg_seed import R


REG_EXTRA = [
    R(
        "liquidity_run_continuation",
        "price_action",
        "A bullish BOS displacement leaves a bullish fair value gap across the broken high, and the retracement respects that gap for continuation.",
        "count(structural_event.kind == bullish_bos AND custom_signals.displacement_candle AND bullish_fvg[0].formed AND bullish_fvg[0].gap_low <= structural_event.reference_price AND bullish_fvg[0].gap_high >= structural_event.reference_price, K_REACTION_WINDOW_CANDLES) >= 1 AND custom_signals.fvg_respected",
        "inferred",
        aliases=["overlapping_fvg_run", "run_continuation"],
        notes="Canonical bullish side. Uses BOS instead of liquidity_sweep_high because the engine sweep detector fires on any breach; the continuation reading needs displacement plus acceptance above the broken high.",
    ),
]


CON_EXTRA = []
