from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "hp_lp_conditions_filter")
    set_category(s, "market_condition_filter", log)
    set_field(s, "summary", "Trade only when higher-timeframe conditions are one-sided with an obvious draw on liquidity. Stand aside in 50/50 conditions, right after a major expansion has taken the draw on liquidity, and before CPI, FOMC or NFP releases.", log)
    s["timeframes"]["bias_tf"] = "4h|1d|1w|1mo"
    log.add("timeframes", "hp_lp_conditions_filter: bias_tf '4h_and_above' -> '4h|1d|1w|1mo' (ladder rungs >= 4h)")
    s["requires_context"]["other"] = ["custom_signals.obvious_draw_on_liquidity", "market:needs_news_calendar"]
    log.add("requires_context", "hp_lp_conditions_filter: prose '10-second rule' -> custom_signals.obvious_draw_on_liquidity; news dependency tagged")
    s["custom_signals"] = [
        ctx.cs("obvious_draw_on_liquidity", "05:05"), ctx.cs("failed_rejection", "07:16"),
        ctx.cs("displacement_candle", "07:16"), ctx.cs("consolidation_expansion_consolidation", "08:06"),
        ctx.cs("major_expansion_dol_swept", "09:09"), ctx.cs("pre_major_news", "10:32"),
    ]
    s["sequence"] = []
    log.add("sequence", "hp_lp_conditions_filter: no signals_required steps, so the 5-line procedure moved into notes")
    set_numeric(s["invalidation"], [None, "custom_signals.failed_rejection"])
    set_numeric(s["filters"], ["NOT custom_signals.pre_major_news", "NOT custom_signals.major_expansion_dol_swept@bias_tf AND NOT custom_signals.consolidation_expansion_consolidation@bias_tf"])
    s["entry_rule"] = keep_rule(s["entry_rule"], structured=None)
    s["target_rule"] = keep_rule(s["target_rule"], structured={"type": "target_level", "reference": "custom_signals.obvious_draw_on_liquidity"})
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "hp_lp_conditions_filter: stop_rule placeholder -> null")
    s["management"]["notes"] = None
    log.add("placeholder_null", "hp_lp_conditions_filter.management.notes 'No management rules discussed.' -> null")
    s["notes"] = ("Conditions filter, not an entry model. Procedure: apply the 10-second rule (1 minute maximum) to the draw on liquidity; "
                  "require clear one-sidedness (arguments for one direction only); skip pre-news and post-expansion states; state an if/then "
                  "demand of price and act only when price meets it exactly. The one-sided bias is the presenter's, not the engine structure bias.")

    e4 = example(doc, "ex4_dollar_post_expansion")
    remap_walkthrough(e4, [None, None, None], log)
    log.add("walkthrough_map", "ex4_dollar_post_expansion: maps_to_signal_step 3 pointed to a step that never existed (signals_required is empty)")
