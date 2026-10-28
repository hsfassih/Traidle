from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "seek_n_destroy")
    set_category(s, "market_condition_filter", log)
    set_field(s, "summary", "Identify a dual-sweep consolidation regime where price takes liquidity on both sides, returns to equilibrium, and PD arrays stop producing follow-through. Stand aside on trend-continuation ideas until a higher-timeframe displacement fair value gap breaks the range.", log)
    s["timeframes"]["bias_tf"] = "1d"
    s["timeframes"]["setup_tf"] = "4h|1d"
    s["timeframes"]["notes"] = ("Use the daily or 4h chart to diagnose the range. The lesson rejects 1-minute fair value gaps as valid exits, so the breakout confirmation must come from the same higher-timeframe regime (4h or 1d).")
    log.add("timeframes", "seek_n_destroy: bias_tf 'Daily' -> '1d'; setup_tf '4h' -> '4h|1d' to reflect the daily/4h higher-timeframe regime examples")
    s["requires_context"]["other"] = ["custom_signals.consolidation"]
    log.add("requires_context", "seek_n_destroy: prose range context -> custom_signals.consolidation; the dual sweep itself is captured by steps 1-2")
    s["glossary"] = {
        "Seek and Destroy": "A dual-sided liquidity trap where price sweeps both sides of a range, returns toward equilibrium, and behaves as consolidation until a higher-timeframe displacement ends it.",
    }
    log.add("glossary", "seek_n_destroy: kept only the presenter jargon term 'Seek and Destroy'; PD-array failure now points to the custom signal")
    s["signals_required"] = [
        step(1, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "01:08"),
        step(2, "structural_event.kind", "equals", "liquidity_sweep_high", "setup_tf", "01:28"),
        step(3, "custom_signals.equilibrium_reversion", "equals", True, "setup_tf", "01:59"),
        step(4, "custom_signals.pd_array_failure", "equals", True, "setup_tf", "03:21"),
    ]
    log.add("signals_required", "seek_n_destroy: added equilibrium-return and PD-array-failure steps; kept the dual sweep as engine structural_event kinds")
    s["custom_signals"] = [
        ctx.cs("consolidation", "00:41"),
        ctx.cs("dealing_range", "01:28"),
        ctx.cs("equilibrium_reversion", "01:51"),
        ctx.cs("pd_array_failure", "02:19"),
        ctx.cs("displacement_candle", "05:45"),
        ctx.cs("htf_range_displacement", "05:45"),
    ]
    s["sequence"] = [
        "Wait for the higher-timeframe range to sweep sell-side liquidity.",
        "Wait for the same expansion to sweep the opposing buy-side liquidity and complete the dealing range.",
        "Mark the 50% equilibrium of that range and wait for price to trade back into it.",
        "Treat the environment as Seek and Destroy once equilibrium holds as consolidation and no follow-through fair value gap appears.",
    ]
    set_numeric(s["invalidation"], ["custom_signals.htf_range_displacement"])
    set_numeric(s["filters"], ["NOT custom_signals.pd_array_failure", "custom_signals.htf_range_displacement"])
    s["entry_rule"] = keep_rule(s["entry_rule"], text="No execution entry is taught here; the lesson only labels the environment and defers turtle-soup execution to the next episode.", structured=None)
    s["target_rule"] = keep_rule(s["target_rule"], text="Price tends to refer back to the 50% equilibrium of the dual-sweep range after both sides are taken.", structured=None)
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "seek_n_destroy: target_rule structured premium_discount.equilibrium -> null because the presenter means the current dual-sweep range midpoint, not the engine latest-swing equilibrium")
    log.add("rules", "seek_n_destroy: stop_rule null text with inferred provenance -> full null rule")
    s["notes"] = ("Conditions filter, not an entry model. The machine proxy treats the presenter's dual-sweep expansion as the current dealing range, then uses a midpoint return plus no follow-through fair value gap as the Seek-and-Destroy condition. A 1-minute gap does not end the regime; the range exit must print as a higher-timeframe displacement fair value gap.")

    e1 = example(doc, "ex1_audusd_consolidation")
    remap_walkthrough(e1, [1, 2, 3, 4], log)
    e2 = example(doc, "ex2_dxy_consolidation_exit")
    remap_walkthrough(e2, [3, 4, None], log)

    for e in (e1, e2):
        for md in e.get("engine_check", {}).get("may_differ") or []:
            old = md.get("signal")
            if old == "pd_array_failure":
                md["signal"] = "custom_signals.pd_array_failure"
                log.add("engine_check", f"{e['example_id']}: may_differ signal '{old}' -> '{md['signal']}'")
            elif old == "htf_range_displacement":
                md["signal"] = "custom_signals.htf_range_displacement"
                log.add("engine_check", f"{e['example_id']}: may_differ signal '{old}' -> '{md['signal']}'")
