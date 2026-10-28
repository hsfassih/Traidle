from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "dealing_range_reversal")
    set_field(
        s,
        "summary",
        "Use daily higher-timeframe PD-array context to pick direction, then on 15m trade a dealing range that has swept both prior swing sides. Enter on the return into the range extreme after both-side liquidity is cleared, with an optional same-direction fair value gap as added confluence.",
        log,
    )
    log.add("mirror", "dealing_range_reversal: rewritten on the canonical bullish side; the walkthrough examples remain mostly bearish")
    s["timeframes"]["bias_tf"] = "1d"
    s["timeframes"]["notes"] = (
        "Daily context frames direction; the 15m chart defines the dealing range and its return entry. "
        "The required bias is the presenter's daily premium/discount-array narrative, not the engine structure bias."
    )
    log.add("timeframes", "dealing_range_reversal: bias_tf '1D' -> '1d'")

    s["requires_context"]["bias"] = ["bullish", "bearish"]
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add(
        "requires_context",
        "dealing_range_reversal: invalid requires_context.bias value replaced with engine-allowed bias set; daily premium/discount-array context moved to custom_signals.htf_pd_array_tap as a proxy",
    )

    s["glossary"] = {
        "Dealing range": "A price range whose low swept the prior swing low and whose high swept the prior swing high; see custom_signals.dealing_range."
    }

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "07:27"),
        step(2, "custom_signals.dealing_range", "equals", True, "setup_tf", "03:21"),
        step(3, "custom_signals.quadrant_extreme", "equals", True, "entry_tf", "10:20"),
        step(4, "custom_signals.fvg_without_displacement", "equals", True, "entry_tf", "06:15", optional=True, maxc=0),
    ]
    log.add(
        "signals_required",
        "dealing_range_reversal: two raw sweep events collapsed into custom_signals.dealing_range; return to the range boundary modeled with custom_signals.quadrant_extreme; optional FVG confluence uses custom_signals.fvg_without_displacement",
    )

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "07:27"),
        ctx.cs("dealing_range", "03:21"),
        ctx.cs("dealing_range_high", "03:21"),
        ctx.cs("dealing_range_low", "03:21"),
        ctx.cs("quadrant_extreme", "10:20"),
        ctx.cs("fvg_without_displacement", "06:15"),
    ]

    s["sequence"] = [
        "Mark the daily premium/discount-array context that defines the draw on liquidity.",
        "On 15m, wait for a dealing range that sweeps both the prior swing high and the prior swing low.",
        "Let price return into the lower quarter of that range for the long-side mirror of the setup.",
        "An optional same-direction fair value gap inside that return adds confluence before entry.",
    ]

    set_numeric(s["invalidation"], ["NOT custom_signals.dealing_range", None])
    set_numeric(s["filters"], ["custom_signals.htf_pd_array_tap", None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the return into the dealing-range extreme in the direction of the higher-timeframe draw on liquidity.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing daily premium/discount array or the daily swing objective identified during the higher-timeframe read.",
        structured=None,
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the dealing-range low on the bullish side (mirror: beyond the high on bearish examples).",
        structured={"type": "beyond", "reference": "custom_signals.dealing_range_low", "buffer_atr": None},
    )
    log.add(
        "rules",
        "dealing_range_reversal: entry/target kept text-only because the presenter gives a boundary idea and daily objective, not one exact machine-fill or target reference; stop anchored to custom_signals.dealing_range_low on the canonical bullish side",
    )

    s["notes"] = (
        "The engine premium_discount fields are not the dealing range used here; the range is the presenter's two-sided liquidity sweep, "
        "modeled with the registry dealing_range/high/low signals. The daily-context proxy uses htf_pd_array_tap, which captures FVG/order-block taps "
        "but does not fully cover previous-day-high/low narrative or every premium/discount-array variant mentioned in the lesson."
    )

    drop_uncertainty(s, "Which dealing range extreme to enter on", "replaced by the canonical bullish-side reading with the return checked via quadrant_extreme", log)
    s["uncertainties"].append(
        {
            "issue": "Daily premium/discount-array context is only partially machine-captured",
            "why": "The presenter mixes daily FVGs, previous-day levels and swing objectives in one narrative. The registry proxy htf_pd_array_tap covers FVG/order-block taps, but not every daily-array variant named in the lesson.",
            "ts": "07:27",
        }
    )
    s["uncertainties"].append(
        {
            "issue": "Entry fill inside the dealing-range extreme is not numerically specified",
            "why": "The lesson says to enter at the dealing-range extreme and shows the lower-quarter idea, but it does not give one exact limit price inside that zone, so the entry rule remains text-only.",
            "ts": "10:20",
        }
    )

    e1 = example(doc, "ex_1_concept_definition")
    remap_walkthrough(e1, [None, 2, None, None, 4], log)

    e2 = example(doc, "ex_2_first_trading_example")
    remap_walkthrough(e2, [1, None, 2, 3, None, 3], log)
    for cso in e2["frames"][0].get("custom_signals_observed") or []:
        if cso.get("name") == "daily_premium_discount_array":
            cso["name"] = "htf_pd_array_tap"
            log.add("custom_alias", "ex_2_first_trading_example: custom_signals_observed 'daily_premium_discount_array' -> 'htf_pd_array_tap'")

    e3 = example(doc, "ex_3_eurusd_losing_curve_simulation")
    remap_walkthrough(e3, [1, 2, 2, 2, None, 3], log)
