from semutil import *


def apply(doc, log, ctx):
    rev = strategy(doc, "new_york_reversal")
    set_field(
        rev,
        "summary",
        "If London fails to reach the higher-timeframe PD array, wait for New York to reach it and reverse from there. Enter on the retracement into the new bullish overlap that forms after the New York displacement.",
        log,
    )
    want_rev_tfs = {
        "bias_tf": "1h|4h|1d|1mo",
        "setup_tf": "15m",
        "entry_tf": "1m|5m",
        "strict": False,
        "notes": "Fractal session model. Use 15m to read the London/New York handoff; execute on 5m with 4h-or-higher context, or on 1m with 1h context.",
    }
    if rev.get("timeframes") != want_rev_tfs:
        rev["timeframes"] = want_rev_tfs
        log.add("timeframes", "new_york_reversal: monthly/4h/5m placeholders -> 1h|4h|1d|1mo bias, 15m setup, 1m|5m entry with paired-fractal note")
    rev["requires_context"]["bias"] = ["bullish", "bearish"]
    rev["requires_context"]["other"] = ["custom_signals.london_missed_htf_poi"]
    log.add("requires_context", "new_york_reversal: prose London-session prerequisite -> custom_signals.london_missed_htf_poi; bias set to presenter HTF context, not engine structure bias")
    rev["glossary"] = {
        "Suffocating the stop loss": "Placing the stop at the intraday low while an unfilled 15m or 1h fair value gap still sits below it, giving price a structural reason to revisit the area.",
    }
    log.add("glossary", "new_york_reversal: registry-backed jargon ('ST model', 'overlapping PD array') removed from glossary; stop-loss phrase kept as presenter wording")
    rev["signals_required"] = [
        step(1, "custom_signals.london_missed_htf_poi", "equals", True, "setup_tf", "06:05"),
        step(2, "custom_signals.ny_taps_htf_poi", "equals", True, "setup_tf", "06:50"),
        step(3, "fvg.bullish", "equals", True, "entry_tf", "06:55"),
        step(4, "custom_signals.overlapping_pd_array", "equals", True, "entry_tf", "07:13", maxc=0),
    ]
    log.add("mirror", "new_york_reversal: symmetric strategy normalized on the canonical bullish side; bearish examples follow the catalog mirror table")
    log.add("signals_required", "new_york_reversal: raw session prose -> London-missed custom context, New-York tap confirmation, bullish FVG, then same-candle overlapping PD array entry zone")
    rev["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "04:05"),
        ctx.cs("first_fvg_after_array_tap", "06:55"),
        ctx.cs("london_missed_htf_poi", "06:05"),
        ctx.cs("ny_taps_htf_poi", "06:50"),
        ctx.cs("overlapping_pd_array", "07:13"),
        ctx.cs("intermediate_term_low", "08:20"),
        ctx.cs("london_taps_htf_poi_and_displaces", "10:30"),
    ]
    log.add("custom_signals", "new_york_reversal: overlap reused from the registry; added reciprocal London-tapped signal for the invalidation that hands the day to New York Retracement")
    rev["sequence"] = [
        "Confirm that London missed the higher-timeframe array.",
        "Wait for New York to trade into that array.",
        "Mark the new bullish fair value gap that starts the reversal leg.",
        "Enter on the retracement into the new overlapping bullish PD array.",
    ]
    set_numeric(
        rev["invalidation"],
        ["custom_signals.london_taps_htf_poi_and_displaces"],
    )
    set_numeric(
        rev["filters"],
        [
            "NOT ((NOT bullish_fvg[0].filled@15m AND bullish_fvg[0].gap_low@15m < custom_signals.intermediate_term_low) OR (NOT bullish_fvg[0].filled@1h AND bullish_fvg[0].gap_low@1h < custom_signals.intermediate_term_low))",
            None,
        ],
    )
    rev["entry_rule"] = keep_rule(
        rev["entry_rule"],
        text="Enter on the retracement into the first overlapping bullish order-block / fair-value-gap zone created by the New York reversal leg.",
        structured={"type": "limit", "reference": "custom_signals.overlapping_pd_array"},
    )
    rev["target_rule"] = keep_rule(
        rev["target_rule"],
        text="Target the opposing bias-timeframe swing high / premium array the New York reversal is drawing toward.",
        structured={"type": "target_level", "reference": "swing_high[0].price@bias_tf"},
    )
    rev["stop_rule"] = keep_rule(
        rev["stop_rule"],
        text="Place the stop beyond the New York intermediate-term low after checking that no unfilled 15m or 1h fair value gap still sits below it.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "new_york_reversal: target 'swing_high' -> scoped bias-timeframe swing_high[0].price; stop 'swing_low' -> custom_signals.intermediate_term_low; overlap kept as the limit entry zone")
    rev["notes"] = (
        "This is the New York version of the London Asia-consolidation reversal: London misses the higher-timeframe array, New York reaches it, and the reversal entry comes from the new overlap that forms after the displacement. "
        "The machine rule encodes the missed-array condition and the overlap entry; the separate claim that New York also makes the day's extreme remains descriptive."
    )
    rev["uncertainties"].append(
        {
            "issue": "New York making the day's extreme is descriptive, not separately encoded",
            "why": "The transcript says New York forms the low/high of the day, but the strategy does not define a machine-safe session-extreme test beyond the higher-timeframe tap and reversal leg.",
            "ts": "06:50",
        }
    )
    remap_walkthrough(example(doc, "usd_jpy_ny_reversal"), [None, 1, 2, 4, None], log)

    retr = strategy(doc, "new_york_retracement")
    set_field(
        retr,
        "summary",
        "If London already reaches the higher-timeframe PD array and displaces, use New York for the continuation retracement instead of a reversal. Buy the new bullish fair value gap, using an overlapping order block only as a tighter refinement when it appears.",
        log,
    )
    want_retr_tfs = {
        "bias_tf": "1h|4h|1d|1mo",
        "setup_tf": "15m",
        "entry_tf": "1m|5m",
        "strict": False,
        "notes": "Fractal continuation model. Use 15m to confirm that London already completed the higher-timeframe tap-and-displacement, then execute the New York pullback on 5m or 1m.",
    }
    if retr.get("timeframes") != want_retr_tfs:
        retr["timeframes"] = want_retr_tfs
        log.add("timeframes", "new_york_retracement: monthly/4h/5m placeholders -> 1h|4h|1d|1mo bias, 15m setup, 1m|5m entry with London-to-New-York continuation note")
    retr["requires_context"]["bias"] = ["bullish", "bearish"]
    retr["requires_context"]["other"] = ["custom_signals.london_taps_htf_poi_and_displaces"]
    log.add("requires_context", "new_york_retracement: prose London-tapped-and-displaced prerequisite -> custom_signals.london_taps_htf_poi_and_displaces; bias set to presenter HTF context")
    retr["glossary"] = {}
    retr["signals_required"] = [
        step(1, "custom_signals.london_taps_htf_poi_and_displaces", "equals", True, "setup_tf", "10:30"),
        step(2, "fvg.bullish", "equals", True, "entry_tf", "11:12"),
        step(3, "custom_signals.overlapping_pd_array", "equals", True, "entry_tf", "12:02", optional=True, maxc=0),
    ]
    log.add("mirror", "new_york_retracement: symmetric strategy normalized on the canonical bullish side; bearish examples follow the mirror table")
    log.add("signals_required", "new_york_retracement: London tap/displacement condensed into one custom context step, followed by the New-York continuation FVG with an optional same-candle overlap refinement")
    retr["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "10:30"),
        ctx.cs("first_fvg_after_array_tap", "10:30"),
        ctx.cs("london_taps_htf_poi_and_displaces", "10:30"),
        ctx.cs("overlapping_pd_array", "12:02"),
        ctx.cs("intermediate_term_low", "11:26"),
        ctx.cs("london_missed_htf_poi", "10:49"),
    ]
    log.add("custom_signals", "new_york_retracement: added reciprocal London-missed signal for invalidation; overlapping_pd_array kept only as an optional refinement")
    retr["sequence"] = [
        "Confirm that London already tapped the higher-timeframe array and displaced from it.",
        "Wait for New York to print a new bullish fair value gap in the continuation direction.",
        "Buy the retracement into that gap, refining to an overlapping PD array only when one appears immediately.",
    ]
    set_numeric(
        retr["invalidation"],
        ["custom_signals.london_missed_htf_poi"],
    )
    set_numeric(retr["filters"], [None])
    retr["entry_rule"] = keep_rule(
        retr["entry_rule"],
        text="Enter on the retracement into the new bullish fair value gap; if a same-timeframe order block overlaps it immediately, use that overlap as the tighter refinement.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    retr["target_rule"] = keep_rule(
        retr["target_rule"],
        text="Target the opposing bias-timeframe swing high / premium array the London displacement points toward.",
        structured={"type": "target_level", "reference": "swing_high[0].price@bias_tf"},
    )
    retr["stop_rule"] = keep_rule(
        retr["stop_rule"],
        text="Place the stop beyond the New York intermediate-term low that anchors the continuation pullback.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "new_york_retracement: entry fvg.ce -> fvg.gap_high (near-edge retracement style); target 'swing_high' -> scoped bias-timeframe swing_high[0].price; stop 'swing_low' -> custom_signals.intermediate_term_low")
    retr["notes"] = (
        "This is the New York version of the London Asia-expansion continuation: London already completes the higher-timeframe tap and displacement, so New York is only a secondary pullback entry. "
        "The overlap with an order block is treated as a preferred refinement, not a separate mandatory model, because the transcript allows taking the new New York fair value gap itself."
    )
    retr["uncertainties"].append(
        {
            "issue": "Overlap refinement is optional rather than mandatory",
            "why": "The walkthrough waits for an overlapping order block and fair value gap, but the strategy text also allows the newly formed New York fair value gap itself; the machine model keeps the overlap as an optional refinement.",
            "ts": "12:02",
        }
    )
    remap_walkthrough(example(doc, "usd_jpy_ny_retracement"), [1, 2, 3, None], log)
