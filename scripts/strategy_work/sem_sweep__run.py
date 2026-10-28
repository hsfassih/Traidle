from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "sweep_or_run")

    set_category(s, "context_framework", log)
    set_field(
        s,
        "summary",
        "Differentiate a reversal sweep from a continuation run by what price does with the fair value gap that drives the breach into liquidity. A sweep closes back through the taken level and resolves into the ST model, while a run keeps the break accepted with a displaced BOS and a respected overlapping continuation FVG.",
        log,
    )

    s["timeframes"] = {
        "bias_tf": None,
        "setup_tf": None,
        "entry_tf": None,
        "strict": False,
        "notes": "Fractal framework. bias_tf is the higher-timeframe draw on liquidity or PD array; setup_tf and entry_tf are the execution chart used to read the breach and the follow-through, and resolve at evaluation time via the contract ladder. The walkthroughs use daily/weekly or 4h context down to 15m execution.",
    }
    log.add("timeframes", "sweep_or_run: placeholder 'htf'/'ltf' timeframes -> null roles resolved by the contract ladder")

    s["requires_context"] = {
        "bias": [],
        "session": [],
        "time_window_ny": None,
        "day_of_week": None,
        "other": ["custom_signals.obvious_draw_on_liquidity"],
    }
    log.add("requires_context", "sweep_or_run: prose draw-on-liquidity prerequisite -> custom_signals.obvious_draw_on_liquidity")

    s["glossary"] = {
        "Sweep": "A liquidity grab that closes back through the taken level instead of accepting beyond it; see custom_signals.pure_sweep.",
        "Run": "A continuation breach accepted with a displaced BOS and a respected overlapping continuation FVG; see custom_signals.liquidity_run_continuation.",
        "Sting": "A shallow first-FVG retracement when price is already very close to the obvious draw on liquidity; this proximity test remains discretionary here.",
    }
    log.add("glossary", "sweep_or_run: registry-backed ST model / overlap terms removed; kept only sweep, run, and sting jargon")

    s["signals_required"] = []
    s["sequence"] = []
    log.add("sequence", "sweep_or_run: no single linear step chain because the framework branches between sweep-reversal and run-continuation readings")

    s["custom_signals"] = [
        ctx.cs("obvious_draw_on_liquidity", "00:13"),
        ctx.cs("pure_sweep", "01:29"),
        ctx.cs("st_model", "03:17"),
        ctx.cs("displacement_candle", "09:46"),
        ctx.cs("fvg_respected", "02:58"),
        ctx.cs("liquidity_run_continuation", "02:58"),
    ]
    log.add("mirror", "sweep_or_run: normalized on the canonical bullish side; bearish sweep examples rely on the catalog mirror table")
    log.add("custom_signals", "sweep_or_run: raw fvg_disrespected / fvg_respected_overlapping / sting_retracement replaced with registry signals plus the new liquidity_run_continuation proxy")

    s["filters"].append(
        {
            "text": "Classify the breach as a sweep only when a pure sweep resolves into the ST model; otherwise require the displaced BOS continuation read with the overlapping FVG.",
            "provenance": "inferred",
            "ts": "02:58",
        }
    )
    set_numeric(
        s["invalidation"],
        ["count(structural_event.kind == bullish_bos AND custom_signals.displacement_candle AND bullish_fvg[0].formed AND bullish_fvg[0].gap_low <= structural_event.reference_price AND bullish_fvg[0].gap_high >= structural_event.reference_price, K_REACTION_WINDOW_CANDLES) >= 1 AND fvg.filled"],
    )
    set_numeric(
        s["filters"],
        [
            None,
            "(count(custom_signals.pure_sweep, K_REACTION_WINDOW_CANDLES) >= 1 AND count(custom_signals.st_model, K_ST_MAX_CANDLES) >= 1) OR custom_signals.liquidity_run_continuation",
        ],
    )
    log.add("filters", "sweep_or_run: sting proximity left text-only; added a machine classifier filter for sweep-reversal vs run-continuation")
    log.add("invalidation", "sweep_or_run: run invalidation proxied as a recent continuation FVG being filled")

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the retracement into the ST model's continuation gap for the sweep branch, or into the continuation FVG that overlaps the broken high for the run branch.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="For a sweep, target the opposing draw on liquidity; for a run, target the still-open higher-timeframe draw on liquidity.",
        structured=None,
    )
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "sweep_or_run: entry/target kept text-only because branch-specific levels differ; unstated stop placeholder -> null")

    resolve_refs(s, {}, log)

    s["notes"] = (
        "Context framework, not one fixed trigger. On the bullish canonical side, a pure_sweep of sell-side liquidity that develops into the ST model is the reversal branch; a displaced bullish BOS that leaves and later respects a bullish FVG across the broken high is the continuation run branch. When price is already close to the obvious draw on liquidity, the presenter only expects a first-FVG sting rather than a deeper overlapping-array retracement."
    )

    drop_uncertainty(s, "Exact definition of the ST Model", "registry st_model now supplies the mechanical proxy used here", log)
    s["uncertainties"].append(
        {
            "issue": "How close price must be to the draw on liquidity for a 'sting' retracement",
            "why": "The presenter contrasts shallow first-FVG reactions with deeper overlap retracements but gives no numeric proximity threshold.",
            "ts": "07:15",
        }
    )

    ex2 = example(doc, "ex2_eu_sweep")
    ex2["frames"][0]["custom_signals_observed"][0]["name"] = "fvg_touched"
    ex2["engine_check"]["may_differ"][0]["signal"] = "custom_signals.fvg_touched"
    log.add("example_signal", "ex2_eu_sweep: sting_retracement observed/may_differ -> custom_signals.fvg_touched (machine can only detect the first-FVG touch, not the proximity judgement)")

    ex3 = example(doc, "ex3_es_sweep_and_run")
    ex3["frames"][0]["custom_signals_observed"][1]["name"] = "liquidity_run_continuation"
    ex3["engine_check"]["may_differ"][0]["signal"] = "custom_signals.liquidity_run_continuation"
    ex3["engine_check"]["may_differ"][0]["reason"] = "The continuation proxy needs a displaced BOS plus an overlapping bullish FVG that later holds; it still cannot prove every discretionary 'stumble higher' reading the presenter sees."
    log.add("example_signal", "ex3_es_sweep_and_run: fvg_respected_overlapping observed/may_differ -> custom_signals.liquidity_run_continuation")

    remap_walkthrough(ex2, [None, None, None, None], log)
    remap_walkthrough(example(doc, "ex1_daily_premium_array_sweep"), [None, None, None], log)
    remap_walkthrough(ex3, [None, None, None, None], log)
    log.add("walkthrough_map", "sweep_or_run: walkthrough step mappings cleared because signals_required is empty and the framework branches between two incompatible trigger paths")
