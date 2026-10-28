from reg_seed import R, INTRADAY

OVERLAP_WICK = "(bullish_fvg[0].gap_high >= open[-1] AND bullish_fvg[0].gap_low <= high[-1])"
F15 = "bullish_fvg[0].filled@15m"
F1H = "bullish_fvg[0].filled@1h"

REG_PILOT = [
    R("presenter_order_block", "candle_pattern",
      "A down candle (or run of consecutive down candles) coming off support whose high is subsequently broken; mirror for bearish.",
      "close[-1] < open[-1] AND high > high[-1]", "inferred",
      notes="Covers the single-candle form broken by the next candle; 'coming off support' is not encoded. Looser than the engine OB, which also needs a same-direction FVG and BOS."),
    R("ob_fvg_overlap_sensitivity", "level",
      "If the order block's wick overlaps a fair value gap, the wick extreme is the entry level; otherwise the body open is.",
      f"{OVERLAP_WICK} * high[-1] + (1 - {OVERLAP_WICK}) * open[-1]", "presenter_stated",
      notes="Frozen at the block-defining step (block candle = t-1); booleans are 1/0 in arithmetic."),
    R("london_kz_entry_trigger", "candle_pattern",
      "In the London kill zone: if Asia expanded in the trade direction, any new fair value gap in the trade direction; if Asia consolidated, the second fair value gap of the ST model.",
      "kill_zone == london AND ((custom_signals.asia_expansion AND bullish_fvg[0].formed) OR (custom_signals.asia_consolidation AND custom_signals.st_model))",
      "presenter_stated", valid_tfs=INTRADAY),
    R("intermediate_term_low_stop", "level",
      "The stop reference beyond which no unmitigated 15m or 1h fair value gap remains (the low that covers higher-timeframe imbalances).",
      f"min(swing_low[0].price, bullish_fvg[0].gap_low@15m * (1 - {F15}) + swing_low[0].price * {F15}, bullish_fvg[0].gap_low@1h * (1 - {F1H}) + swing_low[0].price * {F1H})",
      "inferred", aliases=["intermediate_term_high_stop"],
      notes="Uses the most recent 15m and 1h FVG only; the presenter picks the swing extreme beyond all such gaps."),
    R("failed_rejection", "price_action",
      "Price sweeps a level but does not displace away from it immediately, drifting instead.",
      "count(structural_event.kind == liquidity_sweep_low, K_REACTION_WINDOW_CANDLES + 1) > count(structural_event.kind == liquidity_sweep_low, K_REACTION_WINDOW_CANDLES) AND NOT any(custom_signals.displacement_candle, K_REACTION_WINDOW_CANDLES)",
      "inferred", notes="True K candles after a sweep of a low when no displacement candle away from it followed."),
    R("consolidation_expansion_consolidation", "candle_pattern",
      "Three-candle rhythm: consolidation (or expansion), an expansion candle that creates a fair value gap, then an unpredictable consolidation candle.",
      "body >= K_DISPLACEMENT_BODY_MULT * avg(body[-1], K_DISPLACEMENT_LOOKBACK)", "inferred",
      notes="True at the close of the expansion candle (direction-free): the next candle is the low-probability third candle."),
    R("major_expansion_dol_swept", "price_action",
      "A major expansion has just taken out the higher-timeframe draw on liquidity.",
      "structural_event.kind == liquidity_sweep_high AND custom_signals.displacement_candle", "inferred",
      notes="Evaluated on bias_tf: a displacement candle that sweeps a swing high (buy-side) for longs; the presenter's 'obvious draw' itself is discretionary."),
]
