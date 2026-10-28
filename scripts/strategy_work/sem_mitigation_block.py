from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "mitigation_block_setup")
    set_field(s, "summary", "A bullish mitigation block is the broken swing high of a swing-failure pattern: a higher low holds, then price closes above the prior swing high. Buy the retrace only when that broken high overlaps a bullish fair value gap or bullish order block.", log)
    s["timeframes"]["notes"] = "No fixed timeframes are given; the setup is fractal, and the presenter only says higher-timeframe mitigation blocks are stronger."
    s["requires_context"]["other"] = []
    log.add("requires_context", "mitigation_block_setup: free-prose overlap requirement removed from requires_context.other because step2/filter1 now use custom_signals.mitigation_block_pd_array_overlap")
    s["glossary"] = {}
    log.add("glossary", "mitigation_block_setup: glossary terms removed because swing_failure_pattern and mitigation_block are already covered by registry names/aliases")
    s["signals_required"] = [
        step(1, "custom_signals.swing_failure_pattern", "equals", True, "setup_tf", "01:43"),
        step(2, "custom_signals.mitigation_block_pd_array_overlap", "equals", True, "setup_tf", "02:44"),
        step(3, "candle.low", "lte_signal", "custom_signals.mb_fvg_overlap_sensitivity", "setup_tf", "02:25"),
    ]
    log.add("mirror", "mitigation_block_setup: rewritten on the canonical bullish side; the raw file and examples include bearish explanations and are kept as examples only")
    log.add("signals_required", "mitigation_block_setup: engine order_block.mitigation rejected because the engine never assigns subtype mitigation; step1 stays the registry swing_failure_pattern alias for 'mitigation block'")
    log.add("signals_required", "mitigation_block_setup: generic overlapping_pd_array replaced with mitigation_block_pd_array_overlap because this setup needs the swing-failure mitigation level inside an FVG or order block, not FVG+OB overlap")
    log.add("signals_required", "mitigation_block_setup: added explicit retracement step via candle.low <= custom_signals.mb_fvg_overlap_sensitivity so walkthrough step 3 is machine-addressable")
    s["custom_signals"] = [
        ctx.cs("swing_failure_pattern", "01:43"),
        ctx.cs("mitigation_block_pd_array_overlap", "02:44"),
        ctx.cs("mb_fvg_overlap_sensitivity", "08:27"),
    ]
    s["sequence"] = [
        "Wait for a bullish swing-failure pattern so the broken swing high becomes the mitigation block.",
        "Keep the setup only when that broken high overlaps a bullish fair value gap or bullish order block.",
        "Buy the retrace into the mitigation block, using the wick if the fair value gap overlaps it and otherwise the swing-high candle body open.",
    ]
    set_numeric(s["invalidation"], [None])
    log.add("invalidation", "mitigation_block_setup: invalidation left non-computable because 'without immediate rejection' gives no candle-count or close-back rule")
    set_numeric(s["filters"], ["custom_signals.mitigation_block_pd_array_overlap", "custom_signals.swing_failure_pattern"])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the retrace into the mitigation block: use the broken swing high if a bullish fair value gap overlaps the wick, otherwise use the swing-high candle body open.",
        structured={"type": "limit", "reference": "custom_signals.mb_fvg_overlap_sensitivity"},
    )
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "mitigation_block_setup: target_rule and stop_rule placeholders ('Not explicitly defined...') -> null")
    s["notes"] = ("The presenter’s mitigation block is the broken swing high of a swing-failure pattern, not the engine order-block subtype: "
                  "the engine enum contains mitigation but order_blocks.cpp never assigns it. This rewrite therefore uses the registry "
                  "swing_failure_pattern alias 'mitigation_block' and a separate overlap proxy limited to bullish FVGs and bullish engine order blocks.")
    s["uncertainties"].append({
        "issue": "Entry-level proxy follows the active swing-high candle",
        "why": "The wick-versus-body rule reconstructs the swing-high candle body from price history; if a newer confirmed swing high forms before the retrace, engine-side frozen mitigation-block fields would be needed to keep the original candle locked in.",
        "ts": "08:27",
    })

    for eid in ("ex2_bearish_mb", "ex3_bullish_mb"):
        e = example(doc, eid)
        for md in e["engine_check"]["may_differ"]:
            if md.get("signal") == "entry_level":
                md["signal"] = "custom_signals.mb_fvg_overlap_sensitivity"
                log.add("engine_check", f"{eid}: engine_check.may_differ 'entry_level' -> 'custom_signals.mb_fvg_overlap_sensitivity'")

    e4 = example(doc, "ex4_bullish_mb_ob_confluence")
    for fr in e4.get("frames") or []:
        for cso in fr.get("custom_signals_observed") or []:
            if cso.get("name") == "overlapping_pd_array":
                cso["name"] = "mitigation_block_pd_array_overlap"
                log.add("custom_signals_observed", "ex4_bullish_mb_ob_confluence: overlapping_pd_array -> mitigation_block_pd_array_overlap")
