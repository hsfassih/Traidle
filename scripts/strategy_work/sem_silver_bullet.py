from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "swing_rate_silver_bullet")

    set_field(
        s,
        "summary",
        "Draw swing-rate quartiles (0.25, 0.5, 0.75) between an intermediate-term low and the opposing intermediate-term high. After the 0.5 midpoint behavior is established, buy the first bullish FVG that forms once price reaches 0.75 and target the terminus.",
        log,
    )

    s["timeframes"] = {
        "bias_tf": None,
        "setup_tf": None,
        "entry_tf": None,
        "strict": False,
        "notes": "Fractal framework. The intermediate-term anchor range is taken from the chosen context chart, while setup_tf and entry_tf are the execution chart used to read the 0.5 behavior and the first post-0.75 FVG; if no separate entry chart is used, entry_tf = setup_tf. The examples show 1m inside 1h and 4h inside 1d.",
    }
    log.add("timeframes", "swing_rate_silver_bullet: placeholder HTF/LTF labels -> null roles resolved at evaluation time")

    s["requires_context"] = {
        "bias": ["bullish", "bearish"],
        "session": [],
        "time_window_ny": None,
        "day_of_week": None,
        "other": [
            "custom_signals.intermediate_term_low",
            "custom_signals.intermediate_term_high",
            "custom_signals.swing_rates_framework",
        ],
    }
    log.add("requires_context", "swing_rate_silver_bullet: free-prose anchor requirement -> custom_signals.intermediate_term_low, custom_signals.intermediate_term_high and custom_signals.swing_rates_framework")

    s["glossary"] = {
        "Swing rates": "Fibonacci quartiles (0.25, 0.5, 0.75) drawn between the origin and terminus; see custom_signals.swing_rate_050 and custom_signals.swing_rate_075.",
        "Terminus": "The 1.0 target of the swing-rate range, i.e. the opposing intermediate-term high on the canonical bullish side.",
        "Sting": "A shallow touch into the first fair value gap near 0.75 rather than a deeper overlapping-array retracement.",
    }
    log.add("glossary", "swing_rate_silver_bullet: kept only swing-rates, terminus, and sting jargon; intermediate-term anchors come from registry signals")

    s["signals_required"] = [
        step(1, "custom_signals.swing_rate_050_behavior", "equals", True, "setup_tf", "01:23"),
        step(2, "candle.high", "gte_signal", "custom_signals.swing_rate_075", "setup_tf", "03:02"),
        step(3, "fvg.bullish", "equals", True, "entry_tf", "03:02"),
    ]
    log.add("mirror", "swing_rate_silver_bullet: normalized on the canonical bullish side; bearish walkthroughs rely on the catalog mirror table")
    log.add("signals_required", "swing_rate_silver_bullet: raw 0.75 boolean level -> candle.high gte_signal custom_signals.swing_rate_075; raw fvg.state=open -> first bullish FVG formed after the 0.75 hit")

    s["custom_signals"] = [
        ctx.cs("swing_rates_framework", "00:42"),
        ctx.cs("intermediate_term_low", "00:13"),
        ctx.cs("intermediate_term_high", "00:13"),
        ctx.cs("swing_rate_050", "00:42"),
        ctx.cs("swing_rate_050_behavior", "01:23"),
        ctx.cs("swing_rate_075", "01:53"),
        ctx.cs("consolidation", "01:23"),
    ]
    log.add("custom_signals", "swing_rate_silver_bullet: added registry-backed swing-rate framework, midpoint, 0.75 level, and midpoint behavior signals")

    s["sequence"] = [
        "Identify the intermediate-term low and the opposing intermediate-term high that define the range.",
        "Draw the 0.25, 0.5 and 0.75 swing rates across that range.",
        "Require the deeper 0.5 midpoint behavior: consolidation and/or a liquidity sweep.",
        "Once price reaches 0.75, buy the first bullish fair value gap that forms there.",
        "Target the terminus at the opposing intermediate-term high.",
    ]

    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the first bullish fair value gap that forms once 0.75 is reached.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the terminus at the opposing intermediate-term high.",
        structured={"type": "target_level", "reference": "custom_signals.intermediate_term_high"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Use a tighter, more aggressive stop beyond the first fair value gap because deep retracements are not expected this close to the terminus.",
        structured={"type": "beyond", "reference": "fvg.gap_low", "buffer_atr": None},
    )

    s["management"]["notes"] = "Treat the setup as the final push into the draw on liquidity; once 0.75 is active, expect only shallow pullbacks."
    log.add("rules", "swing_rate_silver_bullet: target swing_extreme -> custom_signals.intermediate_term_high; stop fvg.gap_extreme -> bullish FVG far edge fvg.gap_low")

    s["notes"] = (
        "The file captures only the 0.75 Silver Bullet branch. The presenter also discusses earlier 0.25 entries and deeper 0.5 reactions, but those are context for the quartile map rather than separate normalized triggers here. Required bias is the presenter’s directional range toward the terminus, not the engine BOS/CHoCH bias."
    )

    e1 = example(doc, "nasdaq_structure")
    e1["frames"][0]["custom_signals_observed"][0]["name"] = "swing_rates_framework"
    e1["engine_check"]["may_differ"][0]["signal"] = "custom_signals.swing_rates_framework"
    log.add("example_signal", "nasdaq_structure: fibonacci_swing_rates observed/may_differ -> custom_signals.swing_rates_framework")
