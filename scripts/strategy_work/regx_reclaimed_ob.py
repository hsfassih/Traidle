from reg_seed import R


REG_EXTRA = [
    R(
        "market_maker_curve_shift",
        "price_action",
        "Price taps a higher-timeframe discount array and then prints a bullish structure shift on the setup timeframe, starting the buy curve of the market maker model.",
        "custom_signals.htf_pd_array_tap AND (structural_event.kind == bullish_choch OR structural_event.kind == bullish_bos)",
        "inferred",
        aliases=["market_maker_buy_model", "buy_curve_shift"],
        notes="Proxy for the presenter’s market maker model transition; the full two-curve profile itself is not separately detected.",
    ),
    R(
        "reclaimed_trade_through",
        "price_action",
        "Price closes back above the high of the left-curve bullish order block, reclaiming it as support on the buy curve.",
        "close > order_block.zone_high AND close[-1] <= order_block.zone_high",
        "inferred",
        aliases=["reclaimed_order_block"],
        notes="Uses the bullish order block bound earlier in the strategy steps; bearish examples are the mirror.",
    ),
    R(
        "reclaimed_pd_array_overlap",
        "indicator_relation",
        "The reclaimed bullish order block overlaps a newly formed right-curve PD array: a bullish fair value gap, a later bullish order block, a breaker pattern level, or a swing-failure-pattern mitigation level.",
        "(bullish_fvg[0].confirmed_at > order_block.anchor_at AND max(order_block.zone_low, bullish_fvg[0].gap_low) <= min(order_block.zone_high, bullish_fvg[0].gap_high) AND NOT bullish_fvg[0].filled) OR (bullish_ob[0].anchor_at > order_block.anchor_at AND max(order_block.zone_low, bullish_ob[0].zone_low) <= min(order_block.zone_high, bullish_ob[0].zone_high) AND NOT bullish_ob[0].invalidated) OR (custom_signals.breaker_pattern >= order_block.zone_low AND custom_signals.breaker_pattern <= order_block.zone_high) OR (custom_signals.swing_failure_pattern >= order_block.zone_low AND custom_signals.swing_failure_pattern <= order_block.zone_high)",
        "inferred",
        aliases=["reclaimed_order_block_confluence"],
        notes="Breaker and mitigation-block partners are proxied by the breaker_pattern and swing_failure_pattern level signals; later order-block partners must form after the reclaimed block’s anchor.",
    ),
]
