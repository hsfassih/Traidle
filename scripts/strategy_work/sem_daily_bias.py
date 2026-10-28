from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "daily_bias_via_fvg_reading")
    log.add("category_kept", f"{s['strategy_id']}: worker proposed 'context_framework'; original category '{s['category']}' kept (valid entry category, entry rule present)")
    set_field(
        s,
        "summary",
        "Determine the daily directional bias from how daily fair value gaps behave inside weekly PD-array context. A same-direction daily FVG that is touched and then respected sets the bias for the following days, while filled counter-trend daily FVGs strengthen the opposite draw on liquidity.",
        log,
    )
    s["timeframes"]["bias_tf"] = "1w"
    s["timeframes"]["setup_tf"] = "1d"
    s["timeframes"]["notes"] = (
        "Read daily fair value gaps inside weekly PD-array context. The 1d and 1w candles are Binance UTC-aligned, "
        "not New York-midnight candles; the entry model is separate from this bias framework."
    )
    log.add("timeframes", "daily_bias_via_fvg_reading: bias_tf/setup_tf 'weekly'/'daily' -> '1w'/'1d'; notes clarify UTC-aligned 1d/1w candles")

    s["requires_context"]["bias"] = []
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "daily_bias_via_fvg_reading: removed invalid requires_context.bias 'weekly_fvg'; weekly PD-array context now uses custom_signals.htf_pd_array_tap")

    s["glossary"] = {}
    log.add("glossary", "daily_bias_via_fvg_reading: removed glossary terms 'respect'/'disrespect' because registry signals already define them")

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "setup_tf", "05:05"),
        step(2, "fvg.bullish", "equals", True, "setup_tf", "11:36"),
        step(3, "custom_signals.fvg_touched", "equals", True, "setup_tf", "11:36"),
        step(4, "custom_signals.fvg_respected", "equals", True, "setup_tf", "11:36", maxc=0),
    ]
    log.add("signals_required", "daily_bias_via_fvg_reading: weekly context made explicit with custom_signals.htf_pd_array_tap; raw close>gap_high proxy -> custom_signals.fvg_touched + custom_signals.fvg_respected for the first-reaction confirmation")

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "05:05"),
        ctx.cs("fvg_touched", "05:05"),
        ctx.cs("fvg_respected", "05:05"),
        ctx.cs("htf_opposing_pd_array", "03:27"),
    ]
    log.add("custom_signals", "daily_bias_via_fvg_reading: raw custom signals leg_strength/context_conflict replaced with shared registry signals for weekly context, first touch/respect, and the weekly objective")

    s["sequence"] = [
        "Confirm that price is reacting from the weekly discount array behind the intended daily move.",
        "Mark the new daily bullish fair value gap that defines the current directional leg.",
        "Wait for price to trade back into that daily fair value gap.",
        "Use the first reaction day: if the gap is respected, carry that bias into the following days.",
        "Treat filled counter-trend daily fair value gaps as extra confirmation of the same draw on liquidity.",
    ]

    set_numeric(
        s["invalidation"],
        [
            "fvg.filled",
            None,
        ],
    )
    set_numeric(
        s["filters"],
        [
            "tf_minutes >= 240",
            None,
        ],
    )

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="The entry model is separate from the bias read; once the daily bias is established, any compatible lower-timeframe execution model may be used.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing weekly premium/discount array or the next daily liquidity draw that sits beyond the respected daily FVG.",
        structured={"type": "target_level", "reference": "custom_signals.htf_opposing_pd_array"},
    )
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "daily_bias_via_fvg_reading: stop_rule placeholder -> null; target_rule anchored to custom_signals.htf_opposing_pd_array as the machine-usable weekly objective")

    s["notes"] = (
        "Context framework, not an entry model. The weekly framing is the presenter's premium/discount-array read, not the engine's structure bias. "
        "Filled counter-trend daily FVGs corroborate the active draw on liquidity but are not required as a separate step. "
        "The target text also mentions daily legs with no FVGs left; that alternative remains narrative-only."
    )
    s["uncertainties"].append(
        {
            "issue": "Low-probability weekly context behind the daily FVG is not encoded",
            "why": "The presenter says a daily FVG is weaker when an opposing weekly FVG sits 'right behind it', but he gives no exact distance or overlap rule for that weekly conflict.",
            "ts": "04:42",
        }
    )
    s["uncertainties"].append(
        {
            "issue": "Multiple-day repeated stings before expansion are not quantified",
            "why": "The lesson says a strong daily FVG should react on the first day and that many deep revisits lower the probability, but it gives no exact candle-count or depth threshold.",
            "ts": "07:05",
        }
    )

    e = example(doc, "ex1_daily_bias_fvg_reading")
    remap_walkthrough(e, [None, None, 1, 4], log)
