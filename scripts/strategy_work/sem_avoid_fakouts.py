from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "judas_swing_reversal")
    set_field(
        s,
        "summary",
        "With a higher-timeframe opposing draw on liquidity in view, fade the opening Judas swing when price trades into the first line of defense or sweeps the last line of defense. The first line is usually a fair value gap and the last line is the opposing swing extreme.",
        log,
    )
    s["timeframes"]["bias_tf"] = "4h|1d"
    s["timeframes"]["entry_tf"] = "1m|5m"
    s["timeframes"]["notes"] = (
        "4h or daily context defines the higher-timeframe draw on liquidity; 15m marks the FLOT/LOT area; entries are refined on 1m or 5m. "
        "Written on the canonical bullish side; bearish examples are mirror cases."
    )
    log.add("timeframes", "judas_swing_reversal: bias_tf '4h' -> '4h|1d' and entry_tf '1m' -> '1m|5m' to match the stated 4H-or-daily context and 1m-or-5m execution")
    s["requires_context"]["bias"] = []
    s["requires_context"]["other"] = ["custom_signals.htf_opposing_pd_array", "market:needs_news_calendar"]
    log.add("requires_context", "judas_swing_reversal: presenter draw-on-liquidity moved out of requires_context.bias into custom_signals.htf_opposing_pd_array; news caution kept as a market tag")
    s["glossary"] = {
        "FLOT": "First line of defense: the first retracement PD array, usually a fair value gap; see custom_signals.flod.",
        "LOT": "Last line of defense: the opposing swing extreme that must hold after the manipulation; see custom_signals.llod.",
    }
    log.add("glossary", "judas_swing_reversal: FLOT/LOT canonicalized to the registry-backed flod/llod concepts")
    s["signals_required"] = [
        step(1, "custom_signals.judas_reversal_zone_tap", "equals", True, "setup_tf", "04:33"),
    ]
    log.add("signals_required", "judas_swing_reversal: engine fvg.mitigated replaced with custom judas_reversal_zone_tap so the setup means a FLOT touch or LOT sweep/reclaim during the opening Judas swing")
    s["custom_signals"] = [
        ctx.cs("htf_opposing_pd_array", None),
        ctx.cs("london_open_price", None),
        ctx.cs("midnight_open", None),
        ctx.cs("weekly_open", None),
        ctx.cs("judas_swing", None),
        ctx.cs("flod", "04:33"),
        ctx.cs("llod", "05:33"),
        ctx.cs("opening_judas_swing", "02:48"),
        ctx.cs("judas_reversal_zone_tap", "04:33"),
    ]
    log.add("custom_signals", "judas_swing_reversal: dropped the raw one-off 'Equities Open Judas' object, reused registry open/line-of-defense signals, and added opening_judas_swing + judas_reversal_zone_tap for the combined session/day/week setup")
    s["sequence"] = [
        "With an opposing higher-timeframe draw on liquidity already marked, fade the opening Judas swing when it taps the first line of defense or sweeps the last line of defense and reclaims it.",
    ]
    set_numeric(s["invalidation"], ["NOT bullish_fvg[0].filled", "low >= custom_signals.llod"])
    set_numeric(s["filters"], [None, None])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="For the bullish reading, buy the retracement as the opening Judas swing trades into the first line of defense or briefly sweeps the last line of defense.",
        structured=None,
        prov="presenter_stated",
    )
    s["entry_rule"]["quote"] = "when you have that retracement you want to get involved here to ride that expansion"
    log.add("asr", "judas_swing_reversal.entry_rule.quote: 'write that expansion' -> 'ride that expansion'")
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the higher-timeframe draw on liquidity, typically the first opposing premium array or swing objective above price.",
        structured=None,
        prov="presenter_stated",
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="For the bullish reading, place the stop beyond the last line of defense.",
        structured={"type": "beyond", "reference": "custom_signals.llod", "buffer_atr": None},
        prov="presenter_stated",
    )
    log.add("rules", "judas_swing_reversal: entry/target kept as text because the presenter allows FLOT or LOT entries and multiple higher-timeframe objectives; stop_rule structured to custom_signals.llod")
    s["notes"] = (
        "The higher-timeframe draw on liquidity is the presenter's context, not the engine's BOS/CHoCH bias. "
        "The machine setup uses the registry's London, daily-midnight and Monday-open anchors; the missing ES example's equities-open variant is therefore not separately encoded here. "
        "FLOT uses a touch into the fair value gap, not the engine's CE-based fvg.mitigated event."
    )
    log.add("mirror", "judas_swing_reversal: normalized on the canonical bullish side while keeping the bearish examples as mirrors")
    s["uncertainties"].append(
        {
            "issue": "Average daily volatility filter not quantified",
            "why": "The presenter warns against Judas swings that run too far beyond normal daily volatility, but gives no numeric threshold or anchor for that limit.",
            "ts": "06:34",
        }
    )

    e2 = example(doc, "ex2_forex_session_judas")
    e2["engine_check"]["should_detect"] = []
    e2["engine_check"]["may_differ"] = [
        {
            "signal": "fvg.mitigated",
            "reason": "The presenter treats any trade into the FLOT fair value gap as sufficient, while the engine marks fvg.mitigated only when price reaches the CE midpoint.",
        },
        {
            "signal": "kill_zone",
            "reason": "Presenter uses generic session-open timing rather than only Traidle's fixed kill-zone windows.",
        },
    ]
    log.add("engine_check", "ex2_forex_session_judas: fvg.mitigated moved out of should_detect because the setup is a touch into FLOT, not necessarily an engine CE mitigation")
