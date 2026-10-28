from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "swings_hl")

    log.add("category_kept", f"{s['strategy_id']}: worker proposed 'context_framework'; original category '{s['category']}' kept (valid entry category, entry rule present)")
    set_field(
        s,
        "summary",
        "A lower-timeframe short-term high that forms in front of a higher-timeframe array is often unprotected and gets swept before the higher-timeframe draw is satisfied. Treat the local structure shift as a trap until price actually tags that higher-timeframe array.",
        log,
    )

    s["timeframes"]["bias_tf"] = "15m|1h|4h|1d"
    s["timeframes"]["setup_tf"] = "5m|15m|1h"
    s["timeframes"]["entry_tf"] = None
    s["timeframes"]["notes"] = (
        "Higher-timeframe array/liquidity context against the chart below it. Examples show 5m inside 15m; "
        "the narration extends the same ladder to 15m->1h and 1h->4h, while a Daily FVG/PDH example applies "
        "the same idea with a higher anchor."
    )
    log.add(
        "timeframes",
        "swings_hl: bias_tf/setup_tf vague HTF/LTF labels -> '15m|1h|4h|1d' and '5m|15m|1h'; notes kept as the ladder explanation",
    )

    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add(
        "requires_context",
        "swings_hl: valid structural shift requires the higher-timeframe array to be tagged first -> custom_signals.htf_pd_array_tap",
    )

    s["glossary"] = {
        "Unprotected swing": "A local short-term high or low that forms while price is still reaching for a higher-timeframe array or liquidity pool; expect it to be swept before the higher-timeframe draw is satisfied."
    }

    s["signals_required"] = [
        step(1, "custom_signals.short_term_high", "equals", True, "setup_tf", "07:25"),
        step(2, "structural_event.kind", "equals", "liquidity_sweep_high", "setup_tf", "08:00"),
    ]
    log.add(
        "signals_required",
        "swings_hl: swing_point.is_high -> custom_signals.short_term_high (presenter hierarchy reused); higher-timeframe-array context moved out of steps because 'array just beyond the swing' is only proxied elsewhere",
    )

    s["custom_signals"] = [
        ctx.cs("short_term_high", "07:25"),
        ctx.cs("htf_pd_array_tap", "06:50"),
    ]
    log.add(
        "custom_signals",
        "swings_hl: custom_signals.premium_array replaced by registry short_term_high + htf_pd_array_tap",
    )

    s["sequence"] = [
        "Mark the lower-timeframe short-term high that forms while price is still reaching for the higher-timeframe array.",
        "If that high is swept before the higher-timeframe array is tagged, treat the move as continuation into the array instead of a valid trend change.",
    ]

    set_numeric(s["invalidation"], ["NOT custom_signals.htf_pd_array_tap@bias_tf"])
    set_numeric(s["filters"], ["custom_signals.htf_pd_array_tap@bias_tf"])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="No direct entry trigger is taught; use this lesson as context to avoid taking a local reversal before the higher-timeframe array is tagged.",
        structured=None,
        prov="inferred",
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposite draw on liquidity, such as the opposing discount or premium array.",
        structured=None,
        prov="presenter_stated",
    )
    log.add(
        "rules",
        "swings_hl: entry_rule kept as descriptive context only; target_rule text kept and structured left null because the post-reversal array type/timeframe is not fixed",
    )

    s["notes"] = (
        "Context framework, not a standalone entry model. The machine proxy for 'a nearby higher-timeframe array "
        "just beyond the swing' is custom_signals.htf_pd_array_tap, so PDH/PDL-only magnets and the exact "
        "distance/proximity test remain descriptive context in the examples."
    )
    s["uncertainties"].append(
        {
            "issue": "Exact proximity of the higher-timeframe array to the recent swing",
            "why": "The presenter says the array or liquidity sits 'just beyond' the lower-timeframe swing, but gives no candle-count, ATR, or price-distance rule; the machine proxy only waits for the eventual array tap.",
            "ts": "07:00",
        }
    )

    e2 = example(doc, "ex_2_protected_swings_and_htf")
    remap_walkthrough(e2, [None, 1, 2], log)

    e3 = example(doc, "ex_3_daily_htf_context")
    remap_walkthrough(e3, [None, None], log)
    ec3 = e3.get("engine_check") or {}
    if ec3.get("should_detect") != ["pdh", "fvg"] or ec3.get("may_differ"):
        ec3["should_detect"] = ["pdh", "fvg"]
        ec3["may_differ"] = []
        log.add(
            "engine_check",
            "ex_3_daily_htf_context: 'daily fvg' canonicalized to engine detection 'fvg'; unresolved custom may_differ removed",
        )
