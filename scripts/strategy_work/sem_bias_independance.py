from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "bias_independent")
    set_field(
        s,
        "summary",
        "Use 1-hour intermediate-term structure to set the presenter's draw on liquidity, wait for price to trade into a 1-hour PD array during the presenter's New York AM or PM equity-session windows, and then enter from a lower-timeframe fair value gap. On 1m the presenter wants the second same-direction gap; on 5m one touched same-direction gap is sufficient.",
        log,
    )

    if s["timeframes"].get("entry_tf") != "1m|5m":
        log.add("timeframes", f"bias_independent.entry_tf: {s['timeframes'].get('entry_tf')} -> '1m|5m'")
        s["timeframes"]["entry_tf"] = "1m|5m"
    s["timeframes"]["notes"] = (
        "The 1h chart sets the presenter's draw-on-liquidity context and the 1h PD array; execution is on 1m or 5m. "
        "This is the presenter's directional read, not the engine BOS/CHoCH structure bias."
    )

    s["requires_context"]["session"] = []
    s["requires_context"]["time_window_ny"] = None
    s["requires_context"]["other"] = [
        "custom_signals.htf_pd_array_tap",
        "custom_signals.arjoio_ny_am_session",
        "custom_signals.arjoio_ny_pm_session",
        "market:needs_equity_session",
    ]
    log.add(
        "requires_context",
        "bias_independent: New York AM/PM moved from engine session enums/free prose to custom equity-session rules + market:needs_equity_session; 1h context kept as custom_signals.htf_pd_array_tap",
    )

    s["glossary"] = {
        "8:30/9:30 rule": "If the 08:30 equity-session move already reaches the 1h PD array and displaces from it, the presenter expects the 09:30 open to continue that work rather than engineer a fresh sweep.",
        "1-minute entry pattern": "Uses custom_signals.two_consecutive_fvgs: wait for the second same-direction 1m fair value gap before taking the retracement entry.",
        "5-minute entry pattern": "On 5m, one touched same-direction fair value gap after the 1h array tap is enough; if the first gap never retraces, the next valid 5m gap can be used.",
    }

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "07:20"),
        step(2, "custom_signals.fvg_touched", "equals", True, "entry_tf", "07:44"),
    ]
    log.add(
        "mirror",
        "bias_independent: raw narration/examples are bearish; context, target and stop rewritten on the canonical bullish side via the mirror table",
    )
    log.add(
        "signals_required",
        "bias_independent: raw step fvg.mitigated@1h -> step1 htf_pd_array_tap on bias_tf, step2 lower-timeframe fvg_touched; the 1m second-gap requirement stays descriptive rather than silently equated with an engine bias or FVG state",
    )

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "07:20"),
        ctx.cs("arjoio_ny_am_session", "03:06"),
        ctx.cs("arjoio_ny_pm_session", "03:06"),
        ctx.cs("fvg_touched", "07:44"),
        ctx.cs("intermediate_term_high", "03:35"),
        ctx.cs("intermediate_term_low", "02:32"),
        ctx.cs("short_term_low", "08:41"),
    ]

    s["sequence"] = [
        "Use 1h intermediate-term structure to frame the presenter's draw on liquidity and wait for price to tap the 1h PD array.",
        "Only work inside the presenter's New York AM or PM equity-session windows, not London.",
        "On 1m require the second same-direction fair value gap; on 5m one touched same-direction fair value gap is enough.",
        "Enter on the retracement into that lower-timeframe gap and aim for the opposing 1h intermediate-term swing.",
    ]

    set_numeric(s["invalidation"], ["close@bias_tf < custom_signals.intermediate_term_low@bias_tf"])
    set_numeric(
        s["filters"],
        [
            "custom_signals.arjoio_ny_am_session OR custom_signals.arjoio_ny_pm_session",
            None,
        ],
    )

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="After the 1h PD-array tap, enter on the retracement into the lower-timeframe bullish fair value gap that qualifies the chosen entry model.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing 1h intermediate-term high / premium-array objective.",
        structured={"type": "target_level", "reference": "custom_signals.intermediate_term_high"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the short-term low that forms the lower-timeframe entry swing.",
        structured={"type": "beyond", "reference": "custom_signals.short_term_low", "buffer_atr": None},
    )

    resolve_refs(s, {}, log)
    s["notes"] = (
        "Bias independent means no separate daily-bias filter is required; the working bias is the presenter's 1h draw-on-liquidity read. "
        "That presenter bias is not the engine BOS/CHoCH structure bias. The machine model encodes the 1h PD-array tap, the New York equity-session windows, the lower-timeframe gap touch, and the opposing 1h intermediate-term target, while the 8:30/9:30 sequencing rule and the choice between the 1m second-gap model and the 5m one-gap model remain partly descriptive."
    )
    s["uncertainties"].append(
        {
            "issue": "The 8:30/9:30 rule is not separately encoded",
            "why": "The transcript states that a displacement from the 08:30 equity-session move changes the 09:30 expectation, but it does not define a machine-safe test for exactly what level '08:30 created' that 09:30 should or should not sweep.",
            "ts": "04:47",
        }
    )
    s["uncertainties"].append(
        {
            "issue": "The 1m-versus-5m entry-model choice is only partly machine-encoded",
            "why": "The transcript clearly distinguishes the 1m second-gap model from the 5m one-gap model, but it does not expose a machine-safe binding for which untapped gap remains the chosen entry gap once later gaps form.",
            "ts": "11:42",
        }
    )

    e1 = example(doc, "ex_1_es_1m_strategy_walkthrough")
    e1["frames"][0]["signals_observed"]["bias_source"] = "Presenter's 1h intermediate-term structure draw-on-liquidity read"
    remap_walkthrough(e1, [1, None, None, 2], log)
    e1["engine_check"]["may_differ"] = [
        {
            "signal": "custom_signals.two_consecutive_fvgs",
            "reason": "The presenter requires the second same-direction 1m fair value gap before entry; Traidle does not expose that sequence as a native engine detection.",
        }
    ]

    e2 = example(doc, "ex_2_es_5m_advanced_entry")
    e2["frames"][0]["signals_observed"]["bias_source"] = "Presenter's 1h structure plus the daily-array sweep context"
    remap_walkthrough(e2, [1, None, None, 2], log)
    e2["engine_check"]["may_differ"] = [
        {
            "signal": "custom_signals.fvg_touched",
            "reason": "The presenter allows a touch / sting into the selected 5m fair value gap, whereas Traidle's native engine detection centers on consequent-encroachment mitigation.",
        }
    ]
