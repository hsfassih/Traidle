from semutil import *


def _retarget_may_differ(doc, log):
    mapping = {
        "measured_swing_projection": "custom_signals.measured_swing_projection",
        "standard_deviations": "custom_signals.standard_deviations",
    }
    for e in doc["examples"]:
        for md in (e.get("engine_check") or {}).get("may_differ") or []:
            old = md.get("signal")
            new = mapping.get(old)
            if new:
                md["signal"] = new
                log.add("example_signal", f"{e['example_id']}: engine_check.may_differ '{old}' -> '{new}'")


def apply(doc, log, ctx):
    measured = strategy(doc, "measured_swing_projections")
    set_category(measured, "target_projection", log)
    set_field(
        measured,
        "summary",
        "Project draw-on-liquidity targets from the opening-price anchor to the Judas Swing using the presenter's exact Fibonacci levels. Use only the projection level that overlaps a higher-timeframe PD array, treating wick-versus-body anchor tweaks as descriptive rather than machine-exact.",
        log,
    )
    measured["timeframes"]["bias_tf"] = "4h|1d|1w"
    measured["timeframes"]["setup_tf"] = "1m|5m|15m|30m|1h"
    measured["timeframes"]["entry_tf"] = None
    measured["timeframes"]["notes"] = (
        "The presenter applies the framework to London, daily, weekly, and monthly profiles, but the machine form must rebuild those opening anchors from "
        "intraday 1m-1h candles. bias_tf is the higher-timeframe PD-array chart that the chosen Fibonacci target must overlap."
    )
    log.add("timeframes", "measured_swing_projections: raw profile labels -> intraday setup_tf with 4h|1d|1w bias_tf for machine-readable opening anchors and target arrays")
    measured["requires_context"]["other"] = [
        "custom_signals.judas_swing",
        "custom_signals.overlapping_pd_array",
        "custom_signals.london_open_price",
        "custom_signals.midnight_open",
        "custom_signals.weekly_open",
        "custom_signals.monthly_open",
    ]
    log.add("requires_context", "measured_swing_projections: prose context -> registry Judas Swing, overlap proxy, and eligible London/daily/weekly/monthly opening anchors")
    measured["glossary"] = {
        "Symmetrical swing point": "The -1.0 measured_swing_projection level, i.e. a 1:1 measured move from the selected opening-price anchor through the Judas Swing.",
        "Double symmetrical swing point": "The -2.0 measured_swing_projection level.",
    }
    measured["signals_required"] = [
        step(1, "custom_signals.judas_swing", "equals", True, "setup_tf", "00:46"),
        step(2, "custom_signals.overlapping_pd_array", "equals", True, "bias_tf", "02:58"),
    ]
    log.add("signals_required", "measured_swing_projections: pd_array_overlap -> registry overlapping_pd_array; steps keep the presenter sequence while the exact Fibonacci family remains target-only")
    measured["custom_signals"] = [
        ctx.cs("judas_swing", "00:31"),
        ctx.cs("measured_swing_projection", "02:17"),
        ctx.cs("overlapping_pd_array", "02:58"),
        ctx.cs("london_open_price", "00:58"),
        ctx.cs("midnight_open", "05:28"),
        ctx.cs("weekly_open", "07:36"),
        ctx.cs("monthly_open", "09:28"),
    ]
    measured["sequence"] = [
        "Pick the active opening-price anchor for the profile being traded: London, New York midnight, weekly open, or monthly open.",
        "Measure the Judas Swing away from that anchor and project the presenter's Fibonacci levels from the swing extreme back through the opening price.",
        "Keep only the projection level that overlaps the higher-timeframe PD array acting as draw on liquidity.",
        "Use separate entry models to participate during the distribution phase while holding for that overlapped target.",
    ]
    set_numeric(measured["invalidation"], [None])
    set_numeric(measured["filters"], ["custom_signals.overlapping_pd_array", None])
    measured["entry_rule"] = keep_rule(
        measured["entry_rule"],
        text="This framework supplies the draw-on-liquidity target only; use a separate execution model during the manipulation or retracement phase.",
        structured=None,
        prov="presenter_stated",
    )
    measured["target_rule"] = keep_rule(
        measured["target_rule"],
        text="Target the exact Fibonacci projection level that overlaps the higher-timeframe PD array, using the presenter's stated levels (-0.27, -0.68, -1.0, -1.62, or -2.0).",
        structured={"type": "target_level", "reference": "custom_signals.measured_swing_projection"},
        prov="presenter_stated",
    )
    measured["stop_rule"] = null_rule(measured["stop_rule"])
    log.add("rules", "measured_swing_projections: stop_rule placeholder -> null; target_rule kept as the projection family because one exact overlapping level is chosen case by case")
    resolve_refs(measured, {"ref:daily_profiles": "daily_profiles"}, log)
    measured["notes"] = (
        "The registry Judas Swing is a daily-midnight proxy, so weekly, monthly, and London-profile manipulations remain partially descriptive in the machine form. "
        "The registry overlap proxy is limited to same-timeframe FVG/order-block confluence and does not cover every old high/low or cross-timeframe PD-array combination shown by the presenter. "
        "The presenter corrects the spoken -0.62 level to -0.68 and allows wick-to-wick versus body-to-body anchors to match volatility; those anchor tweaks stay narrative rather than rule-bound."
    )

    deviations = strategy(doc, "standard_deviations_indices")
    set_category(deviations, "target_projection", log)
    set_field(
        deviations,
        "summary",
        "For indices and futures, map draw-on-liquidity targets with the presenter's exact 0.5 standard-deviation Fibonacci levels instead of the standard measured-swing template. Keep only the level that overlaps the intended PD array, and let lower-timeframe scalps cap at nearer structure when the higher-timeframe target is too far away.",
        log,
    )
    deviations["timeframes"]["bias_tf"] = "15m|1h|4h|1d|1w"
    deviations["timeframes"]["setup_tf"] = "1m|5m|15m|30m|1h"
    deviations["timeframes"]["entry_tf"] = None
    deviations["timeframes"]["notes"] = (
        "The presenter frames weekly and daily index profiles on intraday candles, then sometimes drops to 15m for a nearer structural target. "
        "setup_tf therefore stays intraday, while bias_tf can be the nearby target chart or the higher-timeframe PD-array chart."
    )
    log.add("timeframes", "standard_deviations_indices: raw session/daily/weekly wording -> intraday setup_tf with 15m-1w target-array charts")
    deviations["requires_context"]["other"] = [
        "custom_signals.judas_swing",
        "custom_signals.overlapping_pd_array",
        "custom_signals.london_open_price",
        "custom_signals.midnight_open",
        "custom_signals.weekly_open",
    ]
    log.add("requires_context", "standard_deviations_indices: prose context -> registry Judas Swing plus overlap proxy; index/futures market restriction moved to notes")
    deviations["glossary"] = {}
    deviations["signals_required"] = [
        step(1, "custom_signals.judas_swing", "equals", True, "setup_tf", "13:45"),
        step(2, "custom_signals.overlapping_pd_array", "equals", True, "bias_tf", "14:05"),
    ]
    deviations["custom_signals"] = [
        ctx.cs("judas_swing", "13:45"),
        ctx.cs("standard_deviations", "13:15"),
        ctx.cs("overlapping_pd_array", "14:05"),
        ctx.cs("london_open_price", "13:45"),
        ctx.cs("midnight_open", "13:51"),
        ctx.cs("weekly_open", "13:51"),
    ]
    deviations["sequence"] = [
        "Pick the active index/futures opening-price anchor for the profile being traded and identify the Judas Swing away from it.",
        "Project the exact 0.5 standard-deviation Fibonacci levels from that swing.",
        "Retain only the level that overlaps the intended PD array or structural target on the target timeframe.",
        "If trading a lower-timeframe scalp, take profit at the nearer structural objective instead of holding mechanically for the furthest higher-timeframe deviation.",
    ]
    set_numeric(deviations["filters"], [None])
    deviations["entry_rule"] = keep_rule(
        deviations["entry_rule"],
        text="This framework sets the target only; use a separate execution model for the actual index/futures entry.",
        structured=None,
        prov="presenter_stated",
    )
    deviations["target_rule"] = keep_rule(
        deviations["target_rule"],
        text="Target the exact standard-deviation level (-0.5, -1.0, -1.5, -2.0, -2.5, or -3.0) that overlaps the intended PD array or nearby structural objective.",
        structured={"type": "target_level", "reference": "custom_signals.standard_deviations"},
        prov="presenter_stated",
    )
    deviations["stop_rule"] = null_rule(deviations["stop_rule"])
    log.add("rules", "standard_deviations_indices: stop_rule placeholder -> null; target_rule kept as the standard-deviation family because one exact overlapping level is chosen case by case")
    deviations["notes"] = (
        "The presenter separates indices and futures from Forex because their volatility favors 0.5 standard-deviation increments rather than the usual measured-swing levels. "
        "The registry Judas Swing remains a daily-midnight proxy, so weekly-profile manipulations still retain descriptive context around the exact anchor choice. "
        "The registry overlap proxy still only captures same-timeframe FVG/order-block confluence, so other structural overlaps remain descriptive."
    )

    _retarget_may_differ(doc, log)
    for e in doc["examples"]:
        for fr in e.get("frames") or []:
            for fvg in (fr.get("signals_observed") or {}).get("fvgs") or []:
                if fvg.get("presenter_calls_it") == "beautiful rare value Gap":
                    fvg["presenter_calls_it"] = "beautiful fair value gap"
                    log.add("asr", f"{e['example_id']}: 'beautiful rare value Gap' -> 'beautiful fair value gap'")
