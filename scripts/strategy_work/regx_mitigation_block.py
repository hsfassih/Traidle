from reg_seed import R


REG_EXTRA = [
    R(
        "mitigation_block_pd_array_overlap",
        "indicator_relation",
        "The swing-failure-pattern mitigation level lies inside a bullish fair value gap or bullish order block on the same timeframe.",
        "(custom_signals.swing_failure_pattern >= bullish_fvg[0].gap_low AND custom_signals.swing_failure_pattern <= bullish_fvg[0].gap_high AND NOT bullish_fvg[0].filled) OR (custom_signals.swing_failure_pattern >= bullish_ob[0].zone_low AND custom_signals.swing_failure_pattern <= bullish_ob[0].zone_high AND NOT bullish_ob[0].invalidated)",
        "inferred",
        aliases=["mitigation_block_overlap", "mb_pd_array_overlap"],
        notes="Overlap partner proxy is limited to bullish FVGs and bullish engine order blocks; breaker-block and other PD-array partners remain outside this signal.",
    ),
    R(
        "mb_fvg_overlap_sensitivity",
        "level",
        "If the mitigation-block wick overlaps a bullish fair value gap, the broken swing high is the entry level; otherwise the swing-high candle body open is.",
        "(bullish_fvg[0].gap_high >= open[-((open_time - swing_high[0].open_time) / (tf_minutes * 60000))] AND bullish_fvg[0].gap_low <= custom_signals.swing_failure_pattern AND NOT bullish_fvg[0].filled) * custom_signals.swing_failure_pattern + (1 - (bullish_fvg[0].gap_high >= open[-((open_time - swing_high[0].open_time) / (tf_minutes * 60000))] AND bullish_fvg[0].gap_low <= custom_signals.swing_failure_pattern AND NOT bullish_fvg[0].filled)) * open[-((open_time - swing_high[0].open_time) / (tf_minutes * 60000))]",
        "presenter_stated",
        aliases=["mitigation_block_entry_level"],
        notes="Uses the most recent swing high's wick/body geometry on the bullish canonical side; body-open reconstruction comes from the swing-high candle in price history.",
    ),
]

CON_EXTRA = []
