from semutil import *


def apply(doc, log, ctx):
    standard = strategy(doc, "st_model_standard")
    set_field(
        standard,
        "summary",
        "After a higher-timeframe PD-array tap, wait for a displacement FVG, a retracement into that leg, and then a second same-direction FVG. This is the presenter's ST-model replacement for waiting on a traditional MSS entry.",
        log,
    )
    standard["timeframes"] = {
        "bias_tf": None,
        "setup_tf": None,
        "entry_tf": None,
        "strict": False,
        "notes": "Fractal higher-timeframe / lower-timeframe model. bias_tf is the array-defining anchor; setup_tf and entry_tf are the same execution timeframe and resolve at evaluation time via the contract ladder unless the presenter explicitly scales the pair up."
    }
    log.add("timeframes", "st_model_standard: HTF/LTF placeholders -> null role labels with contract-ladder note")
    standard["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "st_model_standard: prose HTF premium/discount prerequisite -> custom_signals.htf_pd_array_tap")
    if standard.get("glossary"):
        standard["glossary"] = {}
        log.add("glossary", "st_model_standard: registry-backed jargon ('ST model', 'overlapping PD array') removed from glossary")
    standard["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "03:36"),
        step(2, "fvg.bullish", "equals", True, "setup_tf", "03:42"),
        step(3, "custom_signals.first_fvg_after_array_tap", "equals", True, "setup_tf", "03:42", maxc=0),
        step(4, "custom_signals.fvg_touched", "equals", True, "setup_tf", "03:49"),
        step(5, "fvg.bullish", "equals", True, "setup_tf", "04:02"),
    ]
    log.add("signals_required", "st_model_standard: HTF tap kept; first FVG bound on an engine step; helper step first_fvg_after_array_tap added; retrace fvg.mitigated -> custom fvg_touched; final bullish FVG binds the entry gap")
    standard["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "03:36"),
        ctx.cs("first_fvg_after_array_tap", "03:42"),
        ctx.cs("fvg_touched", "03:49"),
        ctx.cs("st_model", "04:02"),
        ctx.cs("intermediate_term_high", "04:17"),
        ctx.cs("intermediate_term_low", "10:17"),
    ]
    standard["sequence"] = [
        "Wait for price to trade into the higher-timeframe discount array that sets the reversal.",
        "Mark the first bullish FVG that forms away from that array.",
        "Confirm that this FVG is the first same-direction gap after the higher-timeframe tap.",
        "Wait for price to retrace into that first bullish FVG / current-leg discount array.",
        "Mark the second bullish FVG and use its retracement as the entry setup.",
    ]
    set_numeric(standard["invalidation"], ["count(custom_signals.st_model, K_ST_MAX_CANDLES) == 0"])
    standard["entry_rule"] = keep_rule(
        standard["entry_rule"],
        text="Enter on the retracement into the second bullish FVG; if a breaker overlaps it, that shared zone is the preferred refinement.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    standard["target_rule"] = keep_rule(
        standard["target_rule"],
        text="Target the opposing draw on liquidity, typically the intermediate-term high.",
        structured={"type": "target_level", "reference": "custom_signals.intermediate_term_high"},
    )
    standard["stop_rule"] = keep_rule(
        standard["stop_rule"],
        text="Place the stop beyond the intermediate-term low that anchors the reversal.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "st_model_standard: entry ce -> second FVG near edge; target liquidity_sweep_high -> intermediate_term_high; stop swing_high -> intermediate_term_low (canonical long side)")
    resolve_refs(standard, {}, log)
    standard["notes"] = (
        "The ST model uses FVG sequencing and higher-timeframe PD-array context instead of waiting for structural_event.mss. "
        "The presenter often refines the entry with a breaker/FVG overlap, but the shared breaker edge is not always expressible from the available engine objects, "
        "so the machine rule anchors the limit to the second FVG's near edge."
    )
    standard["uncertainties"].append({
        "issue": "Breaker overlap partner is not always machine-identifiable",
        "why": "The preferred overlapping PD array can be a breaker block rather than an engine order block, so the exact shared edge is not always available from the stored objects.",
        "ts": "04:10",
    })
    remap_walkthrough(example(doc, "ex1_st_model_basic"), [1, 2, 4, 5], log)

    advanced = strategy(doc, "st_model_advanced")
    set_field(
        advanced,
        "summary",
        "After a higher-timeframe PD-array tap, enter on the first same-direction FVG retracement instead of waiting for the full two-FVG ST sequence. It is the aggressive early-entry variation and depends on correct timeframe pairing.",
        log,
    )
    advanced["timeframes"] = {
        "bias_tf": None,
        "setup_tf": None,
        "entry_tf": None,
        "strict": False,
        "notes": "Same higher-timeframe / lower-timeframe structure as the standard ST model. bias_tf is the array-defining anchor; setup_tf and entry_tf are the same execution timeframe and resolve at evaluation time via the contract ladder."
    }
    log.add("timeframes", "st_model_advanced: HTF/LTF placeholders -> null role labels with contract-ladder note")
    advanced["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "st_model_advanced: prose HTF premium/discount prerequisite -> custom_signals.htf_pd_array_tap")
    advanced["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "05:10"),
        step(2, "fvg.bullish", "equals", True, "setup_tf", "05:54"),
        step(3, "custom_signals.first_fvg_after_array_tap", "equals", True, "setup_tf", "05:54", maxc=0),
        step(4, "custom_signals.fvg_touched", "equals", True, "entry_tf", "05:46"),
    ]
    log.add("mirror", "st_model_advanced: written bearish in the transcript examples; normalized on the canonical bullish side because the model is symmetric")
    log.add("signals_required", "st_model_advanced: bearish first-FVG entry -> canonical bullish side; helper step first_fvg_after_array_tap added; retracement into the bound first FVG uses custom fvg_touched")
    advanced["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "05:10"),
        ctx.cs("first_fvg_after_array_tap", "05:54"),
        ctx.cs("fvg_touched", "05:46"),
        ctx.cs("intermediate_term_low", "10:17"),
    ]
    advanced["sequence"] = [
        "Wait for price to trade into the higher-timeframe discount array that sets the reversal.",
        "Mark the first bullish FVG that forms away from that array.",
        "Confirm that this is the first same-direction gap after the higher-timeframe tap.",
        "Buy the retracement into that first bullish FVG instead of waiting for a second gap.",
    ]
    set_numeric(advanced["invalidation"], ["count(custom_signals.first_fvg_after_array_tap, K_TAP_LOOKBACK_CANDLES) == 0"])
    set_numeric(advanced["filters"], ["count(custom_signals.first_fvg_after_array_tap, K_TAP_LOOKBACK_CANDLES) >= 1"])
    advanced["entry_rule"] = keep_rule(
        advanced["entry_rule"],
        text="Enter on the retracement into the first bullish FVG after the higher-timeframe tap, before the full ST-model confirmation.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    advanced["target_rule"] = keep_rule(advanced["target_rule"], text="Target the opposing draw on liquidity.", structured=None)
    log.add("rules", "st_model_advanced: raw target_rule (inferred, no quote) kept as text; no structured form (opposing draw on liquidity is not a catalog price reference)")
    advanced["stop_rule"] = keep_rule(
        advanced["stop_rule"],
        text="Place the stop beyond the intermediate-term low formed at the higher-timeframe array.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "st_model_advanced: entry ce -> first FVG near edge; inferred target removed to null; stop swing_high -> intermediate_term_low (canonical long side)")
    resolve_refs(advanced, {}, log)
    advanced["notes"] = (
        "This is the aggressive first-FVG variation: take the retracement into the first same-direction gap after the higher-timeframe array tap rather than waiting for structural_event.mss or the full ST model. "
        "Written on the bullish side; the transcript examples are bearish."
    )
    advanced["uncertainties"].append({
        "issue": "Exact higher/lower-timeframe alignment rule is not quantified",
        "why": "The presenter says the timeframes must be combined correctly and strongly aligned, but he gives no stricter numeric test beyond using the first FVG after the higher-timeframe tap.",
        "ts": "05:46",
    })
    remap_walkthrough(example(doc, "ex2_eurgbp_advanced"), [1, 2, 3], log)
