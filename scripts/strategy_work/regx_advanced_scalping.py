from reg_seed import R


REG_EXTRA = [
    R(
        "setup_tf_pd_array_tap",
        "price_action",
        "Price trades into the 1-hour or 15-minute boundary used for lower-timeframe execution, such as a fair value gap or order block.",
        "(low <= bullish_fvg[0].gap_high@setup_tf AND high >= bullish_fvg[0].gap_low@setup_tf AND NOT bullish_fvg[0].filled@setup_tf) OR (low <= bullish_ob[0].zone_high@setup_tf AND high >= bullish_ob[0].zone_low@setup_tf AND NOT bullish_ob[0].invalidated@setup_tf)",
        "inferred",
        aliases=["execution_boundary_tap"],
        notes="Proxy limited to the most recent unfilled engine FVG and the most recent non-invalidated engine OB on setup_tf; the daily strong-vs-weak FVG read stays separate context.",
    ),
    R(
        "short_term_swing_point",
        "price_action",
        "The short-term swing point that forms inside the tapped boundary before the new entry fair value gap prints.",
        "swing_low[0].formed",
        "inferred",
        aliases=["st_swing_point"],
        notes="Canonical bullish side uses the newly confirmed swing low on the entry timeframe; the bearish side mirrors to swing_high[0].formed.",
    ),
]


CON_EXTRA = []
