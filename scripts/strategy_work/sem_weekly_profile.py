from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "weekly_profiles_tgif")
    set_category(s, "context_framework", log)
    set_field(
        s,
        "summary",
        "Use the Tuesday or Wednesday higher-timeframe PD-array tap to frame the weekly high or low and ride the continuation through Thursday. If Thursday's expansion looks exhausted, buy the Friday sweep of Thursday's low only after price closes back into the weekly range.",
        log,
    )
    s["timeframes"]["bias_tf"] = "1d"
    s["timeframes"]["setup_tf"] = "4h"
    s["timeframes"]["entry_tf"] = "15m|1h"
    s["timeframes"]["notes"] = (
        "Use the daily chart for the weekly profile and 4h arrays/zones for context; execute the TGIF reversal on 15m or 1h. "
        "Weekly-open context must be derived from intraday candles because 1d and 1w candles are UTC-aligned."
    )
    log.add("timeframes", "weekly_profiles_tgif: bias/setup/entry normalized to 1d, 4h, 15m|1h and notes updated for intraday weekly_open derivation")
    s["requires_context"]["day_of_week"] = ["tuesday", "wednesday", "thursday", "friday"]
    s["requires_context"]["other"] = [
        "custom_signals.htf_pd_array_tap",
        "custom_signals.weekly_open",
        "market:needs_news_calendar",
    ]
    log.add("requires_context", "weekly_profiles_tgif: day_of_week lower-cased; HTF PD-array tap, weekly_open, and news-calendar dependency made explicit")
    s["glossary"] = {
        "TGIF": "Thank God It's Friday: after Thursday stretches the weekly range, Friday can sweep that extreme and reverse back inside the range.",
        "One shot one kill": "Catching most of the weekly range with one macro idea rather than stacking many intraday trades.",
        "Cap the week": "Set the final weekly high or low on Thursday or Friday, ending the expansion phase.",
    }
    log.add("glossary", "weekly_profiles_tgif: Turtle soup removed (registry alias of pure_sweep); weekly-profile jargon kept")
    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "01:36"),
        step(2, "custom_signals.friday_tgif_reversal", "equals", True, "entry_tf", "10:38"),
    ]
    log.add("mirror", "weekly_profiles_tgif: rewritten on the bullish canonical side; bearish weekly-profile variants are obtained via the mirror table")
    log.add("signals_required", "weekly_profiles_tgif: weekly context captured as HTF PD-array tap; TGIF trigger specialized to a Friday sweep/reclaim of Thursday's low")
    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "01:36"),
        ctx.cs("weekly_open", "01:08"),
        ctx.cs("friday_tgif_reversal", "10:38"),
    ]
    log.add("custom_signals", "weekly_profiles_tgif: dropped raw news_event_day, thursday_exhaustion, and fibonacci_weekly_close in favor of registry weekly_open, HTF array context, and reportable discretion")
    s["sequence"] = [
        "Wait for Tuesday or Wednesday to tap the higher-timeframe discount array and set the weekly low.",
        "On Friday, buy only after price sweeps Thursday's low and closes back above it to rebalance the weekly range.",
    ]
    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None, None])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="For the TGIF setup, enter on 15m or 1h after Friday sweeps Thursday's low and closes back into the weekly range.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="For the TGIF reversal, target the first internal rebalance inside the weekly range, typically an unmitigated fair value gap or order block.",
        structured=None,
    )
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "weekly_profiles_tgif: entry/target kept as text-only because the exact reclaim edge and rebalance target are discretionary; placeholder stop_rule -> null")
    resolve_refs(s, {}, log)
    s["notes"] = (
        "The high-volatility day can shift from Tuesday to Wednesday depending on the economic calendar, so the profile is contextual rather than fixed on Sunday. "
        "The Wednesday/Thursday continuation leg is part of the weekly map, but this file only gives a mechanical Friday TGIF reversal trigger. "
        "Weekly_open is kept as contextual anchor only; the presenter also mentions Fibonacci weekly-close projections, but the exact 0.2/0.3 close model is not used for the trade rules here."
    )
    s["uncertainties"] = [
        {
            "issue": "Lack of strict entry mechanics for the weekly expansion leg",
            "why": "The video frames the Tuesday/Wednesday high-volatility day and the Wednesday/Thursday continuation, but it defers the exact continuation entry model to other material.",
            "ts": "12:03",
        },
        {
            "issue": "Thursday exhaustion and Friday close location stay discretionary",
            "why": "The presenter describes Thursday as 'closing poorly' and mentions 0.2/0.3 weekly-close projections, but does not give a precise numeric rule for exhaustion or for which rebalance level Friday must reach.",
            "ts": "12:54",
        },
    ]

    e1 = example(doc, "eu_weekly_profile_tgif")
    e1["frames"][0]["custom_signals_observed"] = []
    log.add("example_cleanup", "eu_weekly_profile_tgif: removed thursday_exhaustion from custom_signals_observed because the exhaustion test stays discretionary and is no longer a strategy custom signal")
    remap_walkthrough(e1, [1, None, None, 2, None], log)

    e2 = example(doc, "dxy_bullish_profile")
    remap_walkthrough(e2, [1, None, None], log)
    e2["engine_check"]["may_differ"] = [
        {
            "signal": "custom_signals.swing_failure_pattern",
            "reason": "The presenter labels an overlapping mitigation block, but the engine has no order_block.mitigation detector; the nearest corpus custom concept is swing_failure_pattern.",
        },
        {
            "signal": "custom_signals.smt_divergence",
            "reason": "SMT confirmation needs a correlated external symbol and remains outside native engine detections.",
        }
    ]
    log.add("engine_check", "dxy_bullish_profile: restored mitigation-block custom mismatch and normalized smt_divergence -> custom_signals.smt_divergence")
