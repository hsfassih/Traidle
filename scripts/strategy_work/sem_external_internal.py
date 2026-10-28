from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "external_internal")
    set_field(
        s,
        "summary",
        "After price trades into a higher-timeframe PD array, wait for a liquidity sweep that breaks the opposing swing with a wick and leaves a fair value gap. Enter on the retracement that respects that confirming gap and target the opposing higher-timeframe objective.",
        log,
    )
    s["timeframes"]["bias_tf"] = None
    s["timeframes"]["setup_tf"] = None
    s["timeframes"]["entry_tf"] = None
    s["timeframes"]["notes"] = (
        "Fractal higher-timeframe array plus execution-chart model. The examples show 1d->15m and 1h/15m->5m, "
        "but the presenter teaches the pattern as transferable across timeframe ladders."
    )
    log.add(
        "timeframes",
        "external_internal: explicit Daily/15m/5m labels generalized to a fractal ladder; the demonstrated 1d->15m and 1h/15m->5m pairings stay in notes",
    )
    s["requires_context"]["bias"] = []
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add(
        "requires_context",
        "external_internal: prose HTF line-of-defense condition -> custom_signals.htf_pd_array_tap; presenter directional context kept out of engine bias",
    )
    s["glossary"] = {
        "EXTERNAL_MOVE": "The liquidity-taking sweep into the higher-timeframe array; machine proxy = structural_event.kind liquidity_sweep_low/high on the setup chart.",
        "INTERNAL_MOVE": "The rebalancing leg that breaks the opposing swing with a wick, leaves a fair value gap, and then respects that gap on the retrace.",
        "LINE_OF_DEFENSE": "The higher-timeframe PD array expected to hold the reversal; the machine proxy is custom_signals.htf_pd_array_tap on bias_tf.",
    }
    log.add(
        "glossary",
        "external_internal: glossary rewritten around the HTF line-of-defense tap, the sweep, and the FVG-confirmed rebalancing leg",
    )
    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "00:31"),
        step(2, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "01:25"),
        step(3, "candle.high", "gt_signal", "custom_signals.short_term_high", "setup_tf", "02:03"),
        step(4, "fvg.bullish", "equals", True, "setup_tf", "02:27"),
        step(5, "custom_signals.fvg_touched", "equals", True, "entry_tf", "05:09"),
    ]
    log.add(
        "mirror",
        "external_internal: written on the canonical bullish side (HTF discount array, liquidity_sweep_low, wick above swing_high, bullish FVG); bearish examples are mirror cases",
    )
    log.add(
        "signals_required",
        "external_internal: HTF prose condition -> custom_signals.htf_pd_array_tap; bullish_choch replaced with candle.high gt_signal custom_signals.short_term_high because the presenter accepts a wick break, not an engine body-close CHoCH",
    )
    log.add(
        "signals_required",
        "external_internal: fvg.state == mitigated -> custom_signals.fvg_touched so the entry uses the presenter's loose respect/touch semantics instead of engine CE mitigation",
    )
    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "00:31"),
        ctx.cs("fvg_touched", "05:09"),
        ctx.cs("htf_opposing_pd_array", "00:40"),
        ctx.cs("short_term_high", "02:03"),
    ]
    s["sequence"] = [
        "Mark the higher-timeframe PD array that should defend the reversal.",
        "Wait for price to sweep liquidity into that array on the execution chart.",
        "Require the next leg to wick through the opposing swing and leave a bullish fair value gap.",
        "Buy the retracement that respects that fair value gap.",
        "Target the opposing higher-timeframe premium array or swing objective.",
    ]
    set_numeric(
        s["invalidation"],
        [
            None,
            "bullish_fvg[0].filled@bias_tf OR bullish_ob[0].invalidated@bias_tf OR fvg.filled",
        ],
    )
    set_numeric(s["filters"], ["fvg.formed", None])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the retracement into the confirming bullish fair value gap; if a breaker or order block overlaps it, use that overlap only as a refinement of the same gap-based entry.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing higher-timeframe premium array first, then any remaining swing-high draw on liquidity beyond it.",
        structured={"type": "target_level", "reference": "custom_signals.htf_opposing_pd_array"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the sweep candle low that made the external move.",
        structured={"type": "beyond", "reference": "structural_event.candle_low", "buffer_atr": None},
    )
    resolve_refs(s, {}, log)
    s["notes"] = (
        "The presenter's market structure shift is modeled as a wick break of the most recent confirmed swing rather than the engine's body-close CHoCH/BOS event. "
        "The higher-timeframe line of defense is proxied by custom_signals.htf_pd_array_tap, and the machine entry uses the confirming FVG's near edge even when the narration also mentions breaker or order-block overlap."
    )
    s["uncertainties"].append(
        {
            "issue": "Presenter FVG threshold may be looser than the engine FVG detector",
            "why": "The narration only requires a fair value gap in the displacement leg, while the machine proxy uses the engine's displacement-qualified FVG because that is the stored level available for entry and invalidation references.",
            "ts": "02:27",
        }
    )

    ex1 = example(doc, "ex1_bullish_daily_15m")
    ex1["engine_check"]["should_detect"] = [
        "structural_event.liquidity_sweep_low",
        "fvg.bullish",
        "fvg.mitigated",
    ]
    ex1["engine_check"]["may_differ"] = [
        {
            "signal": "structural_event.bullish_choch",
            "reason": "The presenter explicitly accepts a wick break as the structure shift here, whereas the engine's CHoCH still needs a body close through the protected high.",
        }
    ]
    remap_walkthrough(ex1, [1, 2, 3, 4, 5], log)

    ex2 = example(doc, "ex2_bearish_es_5m")
    ex2["engine_check"]["should_detect"] = [
        "structural_event.liquidity_sweep_high",
        "fvg.bearish",
        "fvg.mitigated",
    ]
    remap_walkthrough(ex2, [2, None, 4, 5], log)
