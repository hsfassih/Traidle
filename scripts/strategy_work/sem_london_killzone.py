from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "london_killzone")
    set_field(s, "summary", "In the London kill zone, trade toward the opposing 4h swing after a 4h PD array is tapped: enter on a new 5m FVG if Asia expanded, or on the ST model's second FVG if Asia consolidated. The stop goes beyond the intermediate-term extreme that leaves no unmitigated 15m or 1h FVG beyond it.", log)
    s["timeframes"]["notes"] = ("4h PD array context, 15m/1h for stop placement, 5m execution. With daily PD arrays the presenter scales the entry "
                                "timeframe up without naming it (resolve with the contract ladder: ladder_up(entry_tf, 1)).")
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "london_killzone: prose HTF premium/discount condition -> custom_signals.htf_pd_array_tap")
    s["glossary"] = {
        "Intermediate term high (stop)": "In this plan, the swing extreme beyond which no unmitigated 15m or 1h FVG remains; see custom_signals.intermediate_term_low_stop (bullish side).",
    }
    log.add("glossary", "london_killzone: 'ST MODEL' and 'OVERLAPPING PD ARRAY' removed (registry: st_model, overlapping_pd_array); 'INTERMEDIATE TERM HIGH' kept because this presenter means the covering stop extreme")
    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "01:10"),
        step(2, "fvg.bullish", "equals", True, "entry_tf", "01:54"),
        step(3, "custom_signals.london_kz_entry_trigger", "equals", True, "entry_tf", "02:03", maxc=0),
        step(4, "custom_signals.fvg_touched", "equals", True, "entry_tf", "02:11"),
    ]
    log.add("mirror", "london_killzone: written bearish by the presenter; signals/rules rewritten on the canonical bullish side (symmetric: true)")
    log.add("signals_required", "london_killzone: step1 engine bias==bearish -> custom htf_pd_array_tap (presenter bias = 4h array reaction, not engine structure bias); kill_zone + Asia scenario folded into custom london_kz_entry_trigger; fvg.mitigated (engine CE) -> custom fvg_touched; optional second-FVG step folded into st_model")
    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "00:41"), ctx.cs("london_kz_entry_trigger", "02:03"), ctx.cs("asia_expansion", "01:24"),
        ctx.cs("asia_consolidation", "05:27"), ctx.cs("st_model", "05:03"), ctx.cs("fvg_touched", "02:11"),
        ctx.cs("intermediate_term_low_stop", "02:42"),
    ]
    log.add("custom_signals", "london_killzone: st_model_entry -> st_model, intermediate_term_high_stop -> intermediate_term_low_stop (canonical side), overlapping_pd_array dropped (entry uses the new FVG edge)")
    s["sequence"] = [
        "Wait for price to tap the 4h discount array that points to the opposing 4h swing high.",
        "On 5m, mark the new bullish FVG.",
        "Accept it only inside London and if Asia expanded up; if Asia consolidated it must be the ST model's second FVG.",
        "Buy the first retracement into that FVG.",
    ]
    set_numeric(s["invalidation"], [
        "close < custom_signals.intermediate_term_low_stop",
        "(NOT bullish_fvg[0].filled@15m AND bullish_fvg[0].gap_low@15m < swing_low[0].price) OR (NOT bullish_fvg[0].filled@1h AND bullish_fvg[0].gap_low@1h < swing_low[0].price)",
    ])
    set_numeric(s["filters"], [None, None, None])
    s["entry_rule"] = keep_rule(s["entry_rule"],
                                text="In London, enter long on the first retracement into the new bullish FVG (the second FVG when the ST model is required).",
                                structured={"type": "limit", "reference": "fvg.gap_high"})
    s["target_rule"] = keep_rule(s["target_rule"], text="Target the opposing 4h swing high (premium array).",
                                 structured={"type": "target_level", "reference": "swing_high[0].price@bias_tf"})
    s["stop_rule"] = keep_rule(s["stop_rule"],
                               text="Stop below the intermediate-term low that leaves no unmitigated 15m or 1h FVG below it.",
                               structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low_stop", "buffer_atr": None})
    resolve_refs(s, {"ref:order_block": "order_block_mitigation", "ref:daily_profiles": "daily_profiles"}, log)
    s["notes"] = ("Whether Asia expands or consolidates does not invalidate the trade; it decides between the first London FVG and the ST "
                  "model's second FVG. Required bias is the presenter's HTF draw (4h array reaction), not the engine structure bias. "
                  "Written on the bullish side; the presenter's examples are bearish (mirror). A re-entry is valid on another FVG with a new "
                  "intermediate-term extreme toward the same 4h target.")
    s["uncertainties"].append({"issue": "Asia expanded against the trade direction",
                               "why": "The presenter covers Asia expanding away from the HTF array and Asia consolidating, not Asia expanding toward it; the trigger then stays false.",
                               "ts": "01:24"})
    s["uncertainties"].append({"issue": "Machine entry uses the new FVG's near edge",
                               "why": "The quote says 'enter on the overlapping PD array' without naming the overlap partner or edge; the walkthrough fills on the retrace into the new FVG.",
                               "ts": "02:03"})

    for e in doc["examples"]:
        for fr in e["frames"]:
            if fr.get("candles"):
                fr["precision"] = "schematic"
                fr["data_locator_confidence"] = "low"
        e["trade"]["precision"] = "schematic"
        log.add("example_precision", f"{e['example_id']}: reverse-engineered candles and trade prices marked precision 'schematic' (never backtest ground truth)")
    remap_walkthrough(example(doc, "eu_asia_expansion"), [None, 2, 4], log)
    remap_walkthrough(example(doc, "gu_asia_consolidation"), [3, 3, 2, 4], log)
