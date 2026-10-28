from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "overlapping_pd_array_entry")
    set_field(
        s,
        "summary",
        "After a higher-timeframe boundary is tapped, define the retracement context and use OTE only as guidance. Enter on the first overlapping bullish PD array encountered on the pullback, then keep the stop beyond the intermediate-term low instead of tight under the entry.",
        log,
    )

    if s["timeframes"]["bias_tf"] != "1h|1d":
        log.add("timeframes", "overlapping_pd_array_entry: bias_tf '1h' -> '1h|1d' (examples cite 1h context; narration also cites Daily context)")
        s["timeframes"]["bias_tf"] = "1h|1d"
    if s["timeframes"]["entry_tf"] != "1m|5m":
        log.add("timeframes", "overlapping_pd_array_entry: entry_tf '1m' -> '1m|5m' (examples use 1m and 5m execution)")
        s["timeframes"]["entry_tf"] = "1m|5m"
    s["timeframes"]["notes"] = "Examples use 1h context with 1m or 5m execution; Daily context is also mentioned. Equilibrium and OTE frame the pullback but are guidelines, not hard entry thresholds."

    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "overlapping_pd_array_entry: prose HTF 'boundary/context' -> custom_signals.htf_pd_array_tap")

    s["glossary"] = {}
    log.add("glossary", "overlapping_pd_array_entry: glossary cleared because overlapping_pd_array/intermediate_term levels already exist in the registry")

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "01:30"),
        step(2, "custom_signals.overlapping_pd_array", "equals", True, "entry_tf", "07:30"),
    ]
    log.add("mirror", "overlapping_pd_array_entry: symmetric strategy written on the canonical bullish side; bearish cases follow the catalog mirror table")
    log.add("signals_required", "overlapping_pd_array_entry: raw FVG + optional order block steps -> HTF boundary context plus canonical custom_signals.overlapping_pd_array entry trigger")

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "01:30"),
        ctx.cs("overlapping_pd_array", "08:00"),
        ctx.cs("intermediate_term_high", "09:30"),
        ctx.cs("intermediate_term_low", "18:00"),
    ]
    log.add("custom_signals", "overlapping_pd_array_entry: first_overlapping_pd_array -> canonical overlapping_pd_array; added intermediate_term_high/low rule references")

    s["sequence"] = [
        "Wait for price to tap the higher-timeframe boundary that points toward the opposing array.",
        "On the execution chart, enter on the first overlapping bullish PD array encountered on the retracement instead of waiting strictly for OTE.",
    ]
    set_numeric(s["invalidation"], ["close < custom_signals.intermediate_term_low"])
    set_numeric(s["filters"], ["custom_signals.htf_pd_array_tap"])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        structured={"type": "limit", "reference": "custom_signals.overlapping_pd_array"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        structured={"type": "target_level", "reference": "custom_signals.intermediate_term_high"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "overlapping_pd_array_entry: entry reference canonicalized; first stated target anchored to intermediate_term_high; stop reference 'swing_low' -> custom_signals.intermediate_term_low")

    resolve_refs(s, {}, log)

    s["notes"] = (
        "The higher-timeframe boundary is the prerequisite for dropping to the execution chart; without it the presenter considers lower-timeframe overlaps low probability. "
        "The machine target rule captures the first stated target (the immediate intermediate-term high); the later higher-timeframe draw on liquidity stays in text because structured rules allow one target reference. "
        "The canonical overlap proxy is the registry's FVG-plus-order-block confluence, so breaker or mitigation-block examples remain descriptive rather than fully zoned."
    )
    s["uncertainties"].append(
        {
            "issue": "Breaker and mitigation-block overlap examples are proxied through the canonical overlap signal",
            "why": "The registry overlap exposes an FVG plus engine order block zone; the presenter also names breaker and mitigation-block overlaps, but their full block zones are not exposed in the catalog.",
            "ts": "10:30",
        }
    )

    ex1 = example(doc, "ex1_es_overlapping_pd")
    ex1["engine_check"]["may_differ"][0]["signal"] = "custom_signals.overlapping_pd_array"
    log.add("engine_check", "ex1_es_overlapping_pd: may_differ 'first_overlapping_pd_array' -> 'custom_signals.overlapping_pd_array'")
    remap_walkthrough(ex1, [1, 1, None, 2, 2, None], log)

    ex2 = example(doc, "ex2_gbpaud_overlapping_pd")
    ex2["engine_check"]["may_differ"][0]["signal"] = "custom_signals.intermediate_term_low"
    log.add("engine_check", "ex2_gbpaud_overlapping_pd: may_differ 'stop_loss_placement' -> 'custom_signals.intermediate_term_low'")
    remap_walkthrough(ex2, [1, None, 2, None, None], log)
