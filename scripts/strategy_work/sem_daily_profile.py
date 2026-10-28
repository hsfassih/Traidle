from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "daily_profiles")
    set_category(s, "context_framework", log)
    set_field(
        s,
        "summary",
        "Classify the day by which kill zone first taps the higher-timeframe PD array and displaces away from it. If London does it, expect a London-made extreme and a New York retracement or continuation; if London misses, expect New York to tap the array and reverse.",
        log,
    )

    s["timeframes"]["bias_tf"] = "1h|4h|1d"
    s["timeframes"]["notes"] = (
        "Bias comes from the presenter's higher-timeframe PD array read on 1h, 4h or 1d; the profile itself is classified on 15m as Asia, "
        "London and New York interact with that array. Session-dependent rules need intraday data because session windows and NY-time opens are not usable from 1d candles."
    )
    log.add("timeframes", "daily_profiles: bias_tf '1D' -> '1h|4h|1d' (presenter names 1H, 4H and Daily as valid higher-timeframe contexts)")

    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap", "market:needs_news_calendar"]
    log.add("requires_context", "daily_profiles: free-prose economic-calendar dependency -> market:needs_news_calendar; session-array interaction -> custom_signals.htf_pd_array_tap")

    s["glossary"] = {
        "Classic Daily Profile": "Asia consolidates, London taps the higher-timeframe array and creates the daily extreme, then New York retraces and continues from that London move.",
        "New York Reversal": "Asia and London fail to tap the higher-timeframe array, so New York reaches it, creates the daily extreme, and reverses the prior intraday direction.",
        "London Close": "10:00 to 12:00 New York time; in this lesson it is mainly a profit-taking window rather than a fresh continuation entry session.",
    }

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "setup_tf", "16:37"),
        step(2, "custom_signals.displacement_candle", "equals", True, "setup_tf", "06:44", maxc=1),
    ]
    log.add("signals_required", "daily_profiles: raw sweep-only step replaced with session-agnostic HTF-array tap + immediate displacement, which is the common core across London continuation and New York reversal branches")

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "16:37"),
        ctx.cs("displacement_candle", "06:44"),
        ctx.cs("prior_session_expansion", "11:07"),
        ctx.cs("pre_major_news", "01:22"),
        ctx.cs("htf_opposing_pd_array", "07:06"),
    ]
    log.add("custom_signals", "daily_profiles: killzone_interaction_with_htf_array -> htf_pd_array_tap; opposing_premium_array -> htf_opposing_pd_array; prior_session_expansion kept as a new registry signal")

    s["sequence"] = [
        "Mark the higher-timeframe PD array that fits the directional idea for the day.",
        "Watch the 15m Asia, London and New York sessions to see which one first trades into that array and displaces away from it.",
        "If London does it, treat London as the daily extreme and look for New York to retrace and continue that move.",
        "If London misses and New York does it, treat New York as the reversal session and the daily extreme.",
        "Take profit into the opposing higher-timeframe liquidity objective and avoid new continuation entries during London close.",
    ]

    set_numeric(s["invalidation"], ["custom_signals.prior_session_expansion"])
    set_numeric(s["filters"], ["kill_zone != london_close", "NOT custom_signals.pre_major_news", "custom_signals.htf_pd_array_tap"])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter in New York on the retracement after London creates the extreme, or in New York on the reversal after New York itself taps the higher-timeframe array.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing higher-timeframe liquidity pool or opposing premium/discount array.",
        structured={"type": "target_level", "reference": "custom_signals.htf_opposing_pd_array"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the high or low of the session that actually tapped the higher-timeframe array and made the daily extreme.",
        structured=None,
    )
    log.add("rules", "daily_profiles: target_rule opposing_premium_array -> custom_signals.htf_opposing_pd_array; stop_rule kept as text because the tapping session's exact extreme is not a catalog/reference object")

    resolve_refs(s, {}, log)
    log.add("mirror", "daily_profiles: kept on the canonical bullish side; bearish New York-reversal and continuation examples remain as shown and are interpreted through the mirror table")

    s["notes"] = (
        "Context framework, not a standalone entry model: the lesson decides which session should make the high or low of the day, then expects the actual execution pattern to come from another entry model. "
        "The higher-timeframe bias is the presenter's PD-array read, not Traidle's BOS/CHoCH structure bias; the key adaptation is to stop assuming London must make the extreme and instead follow the session that actually taps the array."
    )

    eu = example(doc, "eu_london_continuation")
    if eu["frames"][1]["custom_signals_observed"] == []:
        eu["frames"][1]["custom_signals_observed"] = [{
            "name": "htf_pd_array_tap",
            "candle_i_range": None,
            "measured": {"body": None, "range": None, "lower_wick": None, "upper_wick": None},
            "presenter_calls_it": "daily fair value gap tap in London"
        }]
        log.add("example_custom_signal", "eu_london_continuation: added htf_pd_array_tap to the London setup frame")
    remap_walkthrough(eu, [None, 2, None], log)

    au = example(doc, "au_asian_expansion")
    au["frames"][0]["custom_signals_observed"][0]["name"] = "prior_session_expansion"
    log.add("custom_alias", "au_asian_expansion: kept prior_session_expansion as the canonical session-expansion signal")
    remap_walkthrough(au, [1, 2, None, None], log)

    gu = example(doc, "gu_new_york_reversal")
    gu["frames"][0]["custom_signals_observed"][0]["name"] = "htf_pd_array_tap"
    log.add("custom_alias", "gu_new_york_reversal: killzone_interaction_with_htf_array -> htf_pd_array_tap")
    remap_walkthrough(gu, [None, None, 2, None], log)
