from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "market_structure_secrets")
    set_field(
        s,
        "summary",
        "After price taps a higher-timeframe PD array, the aggressive ST-model entry is the first same-direction FVG away from that tap. The preferred entries are later short-term-range retracements that keep the protected short-term low intact while price targets the opposing intermediate-term high.",
        log,
    )
    s["timeframes"] = {
        "bias_tf": None,
        "setup_tf": None,
        "entry_tf": None,
        "strict": False,
        "notes": "Fractal model. setup_tf is the trading chart, bias_tf is the timeframe immediately above it via the contract ladder, and entry_tf is the same execution chart used for the active short-term range retracement."
    }
    log.add("timeframes", "market_structure_secrets: HTF/LTF placeholders -> null role labels with timeframe-above contract-ladder note")

    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "market_structure_secrets: prose 'navigating between an ITH and an ITL' grounded by higher-timeframe array context -> custom_signals.htf_pd_array_tap")

    if s.get("glossary"):
        s["glossary"] = {}
        log.add("glossary", "market_structure_secrets: glossary cleared because ST model, short-term range, and intermediate-term levels already exist in the registry")

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "12:36"),
        step(2, "custom_signals.first_fvg_after_array_tap", "equals", True, "setup_tf", "14:45", optional=True),
        step(3, "fvg.bullish", "equals", True, "setup_tf", "05:51"),
        step(4, "custom_signals.short_term_range", "equals", True, "setup_tf", "05:51", maxc=0),
        step(5, "custom_signals.fvg_touched", "equals", True, "entry_tf", "08:35"),
    ]
    log.add("mirror", "market_structure_secrets: transcript is taught on the bearish side; strategy rewritten on the canonical bullish side because the range logic is symmetric")
    log.add("signals_required", "market_structure_secrets: presenter market-structure hierarchy kept as custom signals; no BOS/CHoCH/MSS step added because the presenter treats lower-timeframe structure shifts as subordinate to the higher-timeframe PD-array context")
    log.add("signals_required", "market_structure_secrets: optional first_fvg_after_array_tap captures the risky ST-model entry; preferred continuation entries use the confirmed short_term_range plus a retracement into its FVG")

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "12:36"),
        ctx.cs("first_fvg_after_array_tap", "14:45"),
        ctx.cs("short_term_range", "05:51"),
        ctx.cs("fvg_touched", "08:35"),
        ctx.cs("intermediate_term_high", "17:12"),
    ]

    s["sequence"] = [
        "Wait for price to trade into the higher-timeframe discount array that frames the reversal.",
        "The aggressive ST-model variation is the first bullish FVG that forms away from that tap.",
        "For the preferred continuation, wait for a bullish FVG that defines a protected short-term range.",
        "Let price retrace into that short-term-range FVG while the protected short-term low stays intact.",
        "Trade the continuation toward the opposing intermediate-term high on the timeframe above.",
    ]

    set_numeric(s["invalidation"], ["close < custom_signals.short_term_range"])
    set_numeric(s["filters"], ["custom_signals.htf_pd_array_tap"])
    s["invalidation"][0]["text"] = "If price closes below the protecting short-term low, the short-term range is disrespected and the setup is invalid."
    log.add("semantic", "market_structure_secrets.invalidation[0] text mirrored to the canonical bullish side and aligned with the protected short-term low rule")

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the retracement into the bullish FVG that defines the active short-term range.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
        prov="inferred",
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing intermediate-term high.",
        structured={"type": "target_level", "reference": "custom_signals.intermediate_term_high"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop loss beyond the protected short-term low that anchors the active short-term range.",
        structured={"type": "beyond", "reference": "custom_signals.short_term_range", "buffer_atr": None},
    )
    log.add("rules", "market_structure_secrets: bearish target/stop references mirrored to custom_signals.intermediate_term_high and the protected low of custom_signals.short_term_range; entry remains an inferred FVG near-edge retracement")

    s["notes"] = (
        "The presenter’s market structure is a swing-hierarchy framework (intermediate-term highs/lows and short-term ranges), not Traidle’s BOS/CHoCH/MSS state machine. "
        "A lower-timeframe BOS, CHoCH, or MSS may appear inside the move, but it is neither required nor sufficient without the higher-timeframe array context. "
        "The optional first_fvg_after_array_tap step is the aggressive ST-model entry; the preferred trades are the later short-term-range retracements."
    )
    s["uncertainties"].append(
        {
            "issue": "The short-term-range model does not quantify which later range is 'best' once multiple valid ranges form",
            "why": "The presenter says the continuum short-term ranges are the highest-probability trades but does not rank later ranges beyond requiring the higher-timeframe context and the protected range logic.",
            "ts": "09:12",
        }
    )

    ex1 = example(doc, "ex1_nasdaq_basics")
    remap_walkthrough(ex1, [None, None, 4, 5], log)

    ex2 = example(doc, "ex2_audusd_daily_1h")
    remap_walkthrough(ex2, [None, 1, None, 2, 5], log)

    ex3 = example(doc, "ex3_nasdaq_multitimeframe")
    remap_walkthrough(ex3, [None, None, None, None], log)
