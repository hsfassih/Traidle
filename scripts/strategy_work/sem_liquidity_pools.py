from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "liquidity_sweep_and_target")
    set_field(
        s,
        "summary",
        "With higher-timeframe context already set, wait for sell-side liquidity to be swept and reclaimed, then require a strong bullish displacement before targeting opposing buy-side liquidity. Liquidity pools can be equal highs, equal lows or the far side of a consolidation, so the target stays text-only.",
        log,
    )
    s["timeframes"]["bias_tf"] = None
    s["timeframes"]["setup_tf"] = None
    s["timeframes"]["entry_tf"] = None
    s["timeframes"]["notes"] = (
        "Fractal concept with no fixed labels: choose the execution chart for the sweep/reaction, and read higher-timeframe "
        "context from one or more rungs above it via the contract ladder. The canonical bullish rewrite uses sell-side sweep -> "
        "bullish displacement; bearish setups mirror through the catalog mirror table."
    )
    log.add("timeframes", "liquidity_sweep_and_target: bias_tf 'htf' and setup_tf 'ltf' -> null; kept as a fractal HTF-context/LTF-execution concept in notes")

    s["requires_context"]["other"] = []
    log.add("requires_context", "liquidity_sweep_and_target: generic prose HTF-context requirement moved to notes because no exact detector is stated")

    s["glossary"] = {
        "Liquidity pool": "A cluster of stops or resting liquidity, such as equal highs/lows or the far side of a consolidation.",
        "Liquidity run": "A breach that keeps going instead of reclaiming the swept side and displacing away from it.",
    }
    log.add("glossary", "liquidity_sweep_and_target: 'Turtle soup' removed from glossary because it is already the registry alias of custom_signals.pure_sweep")

    s["signals_required"] = [
        step(1, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "12:21"),
        step(2, "custom_signals.pure_sweep", "equals", True, "setup_tf", "12:21", maxc=0),
        step(3, "custom_signals.displacement_reaction", "equals", True, "setup_tf", "13:08"),
    ]
    log.add("mirror", "liquidity_sweep_and_target: raw bullish-side sweep-high narrative rewritten on the canonical bullish side (sell-side sweep -> buy-side target); bearish setups mirror")
    log.add("signals_required", "liquidity_sweep_and_target: engine liquidity_sweep_* fires on any breach, so the setup now requires the sweep event, same-candle pure_sweep reclaim, then displacement_reaction")

    s["custom_signals"] = [
        ctx.cs("pure_sweep", "12:21"),
        ctx.cs("displacement_candle", "13:08"),
        ctx.cs("displacement_reaction", "13:08"),
        ctx.cs("failed_rejection", "13:08"),
    ]
    log.add("custom_signals", "liquidity_sweep_and_target: reused registry pure_sweep, displacement_candle, displacement_reaction and failed_rejection; dropped orphan raw consolidation_range")

    s["sequence"] = [
        "Wait for sell-side liquidity to be swept on the execution chart.",
        "Require the sweeping candle to close back above the swept level.",
        "Enter only after a strong bullish displacement confirms the rejection, targeting opposing buy-side liquidity.",
    ]

    set_numeric(s["invalidation"], ["custom_signals.failed_rejection"])
    set_numeric(s["filters"], ["custom_signals.displacement_reaction", None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter long only after the reclaimed sweep produces a clear bullish displacement/reaction.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target opposing buy-side liquidity such as equal highs or the far side of the consolidation that built the sweep.",
        structured=None,
    )
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "liquidity_sweep_and_target: target_rule reference liquidity_levels -> text-only; stop_rule placeholder -> null")

    s["notes"] = (
        "The presenter teaches a market-mechanics concept rather than a fixed PD-array execution model. A valid sweep needs the registry's pure_sweep reclaim because "
        "the engine liquidity_sweep_* signal itself fires on wick or body breaches. The exact higher-timeframe context and the exact opposing target pool remain discretionary."
    )
    s["uncertainties"].append(
        {
            "issue": "Exact higher-timeframe context",
            "why": "The presenter insists on higher-timeframe context or a PD array, but does not state one exact measurable detector or a fixed timeframe pair.",
            "ts": "12:21",
        }
    )

    for eid in ("ex3_fractal_bullish_breaker", "ex4_bullish_breaker_continuation"):
        if any(e.get("example_id") == eid for e in doc["examples"]):
            drop_example(doc, eid, "mispasted breaker-block example payload from another strategy file", log)
