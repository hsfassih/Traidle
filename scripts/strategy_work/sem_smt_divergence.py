from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "smt_divergence_confirmation")
    set_field(
        s,
        "summary",
        "Use SMT divergence only after price reaches a higher-timeframe PD array on the traded instrument. In the bullish form, the traded instrument sweeps a prior low while a correlated external symbol holds a higher low, then the buy-stop entry sits above the sweeping candle high.",
        log,
    )
    s["timeframes"]["notes"] = (
        "Fractal confirmation model. The higher-timeframe context is a presenter-defined PD array on the traded instrument "
        "(examples: 4h or 1d), while the sweep and the SMT comparison are read from the lower execution chart."
    )
    s["requires_context"]["bias"] = []
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap", "market:needs_external_symbols"]
    log.add(
        "requires_context",
        "smt_divergence_confirmation: requires_context.other 'htf_pd_array' -> custom_signals.htf_pd_array_tap and market:needs_external_symbols; presenter bias kept in notes",
    )
    s["glossary"] = {
        "SMT": "Smart Money Technique: a crack in correlation between correlated instruments; see custom_signals.smt_divergence.",
        "Buying on strength": "Place a stop order above the high of the sweep candle; machine reference = structural_event.candle_high.",
    }
    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "02:18"),
        step(2, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "03:44"),
        step(3, "custom_signals.smt_divergence", "equals", True, "setup_tf", "04:07"),
    ]
    log.add(
        "signals_required",
        "smt_divergence_confirmation: HTF PD-array context restored as step1, single-symbol sweep kept as step2, and bullish_smt_divergence normalized to registry custom_signals.smt_divergence for the cross-symbol confirmation step",
    )
    s["custom_signals"] = [ctx.cs("htf_pd_array_tap", "02:18"), ctx.cs("smt_divergence", "04:07")]
    log.add(
        "custom_signals",
        "smt_divergence_confirmation: dropped non-registry buying_on_strength_trigger because structural_event.candle_high already expresses the trigger level exactly",
    )
    s["sequence"] = [
        "Wait for the traded instrument to tap the higher-timeframe PD array already chosen by the bias process.",
        "On the execution chart, let the traded instrument sweep the prior swing low.",
        "Confirm SMT divergence against the correlated symbol, then place the buy stop above the sweep candle high or wait for a separate lower-timeframe entry model.",
    ]
    set_numeric(s["invalidation"], ["NOT custom_signals.smt_divergence"])
    set_numeric(s["filters"], ["custom_signals.htf_pd_array_tap", "structural_event.kind == liquidity_sweep_low", None])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Place a buy stop above the high of the sweep candle ('buying on strength'), or wait for a later lower-timeframe entry model.",
        structured={"type": "stop", "reference": "structural_event.candle_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing swing high or the higher-timeframe PD array already serving as the draw on liquidity.",
        structured=None,
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop below the sweep candle low with enough room to breathe; the quoted 6-point NQ example stays market-specific.",
        structured={"type": "beyond", "reference": "structural_event.candle_low", "buffer_atr": "K_STOP_BUFFER_ATR"},
    )
    s["notes"] = (
        "SMT is a late confirmation inside an existing higher-timeframe PD array, not a standalone bias tool; the required bias remains "
        "the presenter's higher-timeframe read rather than the engine's structure bias. Because Traidle stores one symbol per process, "
        "the correlation leg and the strength-leader choice remain external: custom_signals.smt_divergence uses external.correlated.* and "
        "this strategy is therefore tier C. Candle overlays matter because line charts hide the wick sweeps that the confirmation depends on."
    )
    s["uncertainties"].append(
        {
            "issue": "Strength-leader selection stays external",
            "why": "Choosing which correlated instrument to trade requires cross-symbol higher-timeframe comparison beyond the traded symbol's stored state.",
            "ts": "07:01",
        }
    )
    s["uncertainties"].append(
        {
            "issue": "Target selection stays discretionary",
            "why": "The presenter allows either the opposing swing or the higher-timeframe PD array as the draw on liquidity, so the text is preserved and structured null avoids inventing a single target reference.",
            "ts": "10:31",
        }
    )
    log.add("mirror", "smt_divergence_confirmation: rewritten on the canonical bullish side; bearish use is implied by the mirror table")

    e1 = example(doc, "smt_indices_es_nq")
    e1["frames"][1]["custom_signals_observed"] = [
        x for x in e1["frames"][1].get("custom_signals_observed") or [] if x.get("name") == "smt_divergence"
    ]
    e1["engine_check"]["should_detect"] = ["fvg.bullish", "structural_event.liquidity_sweep_low", "swing_low"]
    log.add(
        "example_cleanup",
        "smt_indices_es_nq: removed buying_on_strength_trigger from custom_signals_observed and narrowed engine_check.should_detect swing_point -> swing_low",
    )
    remap_walkthrough(e1, [1, None, 3, 3, None, None], log)

    e2 = example(doc, "smt_forex_gu_eu")
    remap_walkthrough(e2, [1, None, 3, 3, None], log)
