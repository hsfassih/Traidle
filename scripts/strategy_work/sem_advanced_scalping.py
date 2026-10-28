from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "htf_boundary_scalping")
    set_category(s, "htf_continuation", log)
    set_field(
        s,
        "summary",
        "Use the daily strong-vs-weak fair value gap read only as directional context, then wait for price to tap a 1-hour or 15-minute PD array before drilling into 1-minute or 5-minute execution. Enter only after a short-term swing point forms inside that boundary and a new same-direction fair value gap prints.",
        log,
    )
    s["timeframes"]["bias_tf"] = "1d"
    s["timeframes"]["setup_tf"] = "15m|1h"
    s["timeframes"]["entry_tf"] = "1m|5m"
    s["timeframes"]["notes"] = (
        "Daily bias is descriptive only here: the presenter reads strong vs weak daily fair value gaps, but the actionable "
        "boundary tap happens on the 1-hour or 15-minute setup_tf and execution remains on the 1-minute or 5-minute entry_tf."
    )
    log.add("timeframes", "htf_boundary_scalping: normalized 1D / 1H or 15m / 1m or 5m -> 1d / 15m|1h / 1m|5m")

    s["requires_context"]["bias"] = ["bullish", "bearish"]
    s["requires_context"]["session"] = ["london", "new_york_am"]
    s["requires_context"]["other"] = ["custom_signals.setup_tf_pd_array_tap", "market:needs_news_calendar"]
    log.add("requires_context", "htf_boundary_scalping: daily bias kept as presenter context; news-events prose moved from session enum to market:needs_news_calendar")

    s["glossary"] = {
        "Boundary": "The 1-hour or 15-minute PD array that must be tapped before the lower-timeframe scalp is valid; see custom_signals.setup_tf_pd_array_tap.",
        "ST": "The short-term swing point that forms inside the boundary before the new entry fair value gap prints; see custom_signals.short_term_swing_point.",
    }

    s["signals_required"] = [
        step(1, "custom_signals.setup_tf_pd_array_tap", "equals", True, "setup_tf", "09:18"),
        step(2, "custom_signals.short_term_swing_point", "equals", True, "entry_tf", "12:08"),
        step(3, "fvg.bullish", "equals", True, "entry_tf", "12:13"),
    ]
    log.add("mirror", "htf_boundary_scalping: raw narration/examples are mostly bearish; machine rewrite kept the canonical bullish side because the strategy is symmetric")
    log.add("signals_required", "htf_boundary_scalping: boundary_pd_array -> setup_tf_pd_array_tap; st_formation -> short_term_swing_point; fvg -> fvg.bullish")

    s["custom_signals"] = [
        ctx.cs("setup_tf_pd_array_tap", "09:18"),
        ctx.cs("short_term_swing_point", "12:08"),
        ctx.cs("intermediate_term_low", "12:27"),
    ]

    s["sequence"] = [
        "Wait for price to tap the 1-hour or 15-minute discount array that aligns with the daily fair value gap context.",
        "On 1-minute or 5-minute execution, wait for the short-term swing low that forms inside that boundary.",
        "Buy the new bullish fair value gap that prints after that swing, using any overlap refinement only inside that gap zone.",
    ]

    set_numeric(s["invalidation"], ["NOT custom_signals.setup_tf_pd_array_tap"])
    set_numeric(s["filters"], [None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter long on the new bullish 1-minute or 5-minute fair value gap; if the retracement shares price with the swing-low area, use that overlap only as the refinement.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Take an initial 1:2 reward-to-risk objective.",
        structured={"type": "rr_multiple", "reference": 2.0},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the intermediate-term low that formed before the entry fair value gap.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "htf_boundary_scalping: entry -> new FVG near edge, target -> rr_multiple 2.0, stop -> intermediate_term_low")

    if s.get("reinforces"):
        s["reinforces"] = []
        log.add("ref_removed", "htf_boundary_scalping.reinforces: 'st_model' names a concept spanning multiple strategies, not one unambiguous strategy_id")

    s["notes"] = (
        "The daily bias is the presenter's strong-vs-weak fair value gap read, not the engine structure bias. "
        "The machine setup therefore anchors the required boundary on setup_tf and leaves the daily bias as descriptive context. "
        "The news-release variation remains text only because the raw file names NFP as an example but does not define which releases qualify."
    )
    s["uncertainties"] = [
        {
            "issue": "Daily strong-vs-weak fair value gap bias is descriptive only",
            "why": "The presenter explains bias through which daily fair value gap is strong or weak, but this file gives no numeric rule for ranking one daily gap against another.",
            "ts": "05:43",
        },
        {
            "issue": "Overlap partner in the entry quote is unspecified",
            "why": "The quote mentions using the overlapping part with the lows/highs, but it does not name a second PD-array partner; the machine rule therefore uses the new fair value gap edge as the structural entry.",
            "ts": "12:20",
        },
    ]

    for e in doc["examples"]:
        for fr in e.get("frames") or []:
            for cso in fr.get("custom_signals_observed") or []:
                if cso.get("name") == "st_model":
                    cso["name"] = "short_term_swing_point"
                    log.add("example_signal", f"{e['example_id']}: custom_signals_observed 'st_model' -> 'short_term_swing_point' (raw ST here is the swing point, not the full ST model)")
