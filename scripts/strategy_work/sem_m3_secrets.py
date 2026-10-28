from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "m3_secrets")

    set_field(
        s,
        "summary",
        "Use higher-timeframe PD-array context to trade the lower-timeframe continuation from an early ST-model FVG into later short-term-range retracements. The preferred target is the opposing higher-timeframe PD array while the protected short-term low remains intact.",
        log,
    )

    s["timeframes"] = {
        "bias_tf": "1mo|1w|1d|1h",
        "setup_tf": "1d|4h|1h|15m",
        "entry_tf": "1h|15m|5m",
        "strict": False,
        "notes": "Fully fractal. setup_tf is the first lower timeframe used to confirm the reaction off bias_tf, and entry_tf is the same chart or the next lower contract-ladder rung used for the short-term-range retracement (for example 1mo -> 1d/4h, 1w -> 1h, 1h -> 5m).",
    }
    log.add("timeframes", "m3_secrets: normalized timeframe labels and kept the presenter's fractal bias/setup/entry ladder")

    s["requires_context"]["bias"] = []
    s["requires_context"]["day_of_week"] = None
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap", "market:needs_news_calendar"]
    log.add("requires_context", "m3_secrets: presenter HTF context -> custom_signals.htf_pd_array_tap; news-driver requirement tagged as market:needs_news_calendar; day_of_week prose moved out of the enum field")

    s["glossary"] = {
        "Magnet and rocket": "Presenter label for a higher-timeframe fair value gap that first attracts price as the draw on liquidity and can then reject sharply once reached; the machine target uses custom_signals.htf_opposing_pd_array."
    }
    log.add("glossary", "m3_secrets: CONTEXT/ST/SHORT_TERM_RANGE removed because the registry already defines them; kept 'Magnet and rocket' as presenter jargon")

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "02:49"),
        step(2, "custom_signals.first_fvg_after_array_tap", "equals", True, "setup_tf", "06:56", optional=True),
        step(3, "fvg.bullish", "equals", True, "entry_tf", "07:17"),
        step(4, "custom_signals.short_term_range", "equals", True, "entry_tf", "07:17", maxc=0),
        step(5, "custom_signals.fvg_touched", "equals", True, "entry_tf", "19:52"),
    ]
    log.add("signals_required", "m3_secrets: htf_discount_array_tap -> canonical htf_pd_array_tap alias; ST expressed as optional first_fvg_after_array_tap; execution grounded by entry_tf FVG + short_term_range + retracement touch")

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "02:49"),
        ctx.cs("first_fvg_after_array_tap", "06:56"),
        ctx.cs("short_term_range", "07:17"),
        ctx.cs("fvg_touched", "19:52"),
        ctx.cs("htf_opposing_pd_array", "03:14"),
        ctx.cs("consolidation", "01:48"),
    ]
    log.add("custom_signals", "m3_secrets: canonicalized htf_discount_array_tap -> htf_pd_array_tap, st_entry -> first_fvg_after_array_tap, and fvg_magnet_rocket -> htf_opposing_pd_array")

    s["sequence"] = [
        "Wait for price to tap the higher-timeframe discount array that sets the directional draw.",
        "The aggressive ST-model variation is the first bullish FVG that forms away from that tap.",
        "For the preferred trade, wait for a later bullish FVG that defines a protected short-term range on the execution chart.",
        "Let price retrace into that short-term-range FVG while the protected low stays intact.",
        "Trade the continuation toward the opposing higher-timeframe premium array.",
    ]

    set_numeric(
        s["invalidation"],
        [
            "close < custom_signals.short_term_range",
            None,
        ],
    )
    set_numeric(
        s["filters"],
        [
            "custom_signals.htf_pd_array_tap",
            None,
            None,
            "NOT custom_signals.consolidation",
        ],
    )

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the retracement into the bullish FVG that defines the active short-term range, especially when it overlaps an order block or breaker block.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing higher-timeframe premium array; the presenter also cites the opposing intermediate-term high and a simple 1:2 R:R as discretionary profit-taking alternatives.",
        structured={"type": "target_level", "reference": "custom_signals.htf_opposing_pd_array"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop loss beyond the protected short-term low that defines the active short-term range and leaves price no reason to return.",
        structured={"type": "beyond", "reference": "custom_signals.short_term_range", "buffer_atr": None},
    )
    log.add("rules", "m3_secrets: entry normalized to the short-term-range FVG near edge, target to the canonical HTF opposing PD array, and stop to the protected short-term low")

    resolve_refs(
        s,
        {
            "ref:intermediate_term_structure": "market_structure_secrets",
            "ref:short_term_ranges": "market_structure_secrets",
        },
        log,
    )

    s["notes"] = (
        "The Market Maker Model here is a macro context framework, not a rigid engine pattern. "
        "The presenter’s directional draw is the higher-timeframe PD-array reaction, not Traidle’s BOS/CHoCH bias state. "
        "The optional first_fvg_after_array_tap step is the earlier ST-model entry; the preferred trades are the later short-term-range retracements. "
        "Order-block or breaker-block overlap is confirmatory commentary, not a separate mandatory detector in this rewrite."
    )

    ex1 = example(doc, "ex1_usdjpy_monthly_daily")
    remap_walkthrough(ex1, [1, None, 2, 4], log)

    ex2 = example(doc, "ex2_usdjpy_weekly_1h_5m")
    remap_walkthrough(ex2, [None, 1, 4, None], log)
    ex2["engine_check"] = {
        "should_detect": ["fvg.bullish"],
        "may_differ": [
            {
                "signal": "custom_signals.htf_pd_array_tap",
                "reason": "Requires cross-timeframe integration between the weekly array and the 1H/5m execution charts.",
            },
            {
                "signal": "order_block.subtype",
                "reason": "The presenter treats the 1H overlap as a breaker-block discount array; the engine breaker subtype may capture only a subset of that context.",
            },
        ],
    }
    log.add("engine_check", "ex2_usdjpy_weekly_1h_5m: rewrote engine_check to valid catalog/custom signals after raw should_detect listed a non-detection breaker shorthand")

    ex3 = example(doc, "ex3_eurgbp_market_maker_buy_model")
    ex3["frames"][0]["custom_signals_observed"] = [
        {
            "name": "htf_opposing_pd_array",
            "candle_i_range": [0, 0],
            "measured": {
                "body": None,
                "range": None,
                "lower_wick": None,
                "upper_wick": None,
            },
            "presenter_calls_it": "weekly fair value gap acting as the magnet",
        }
    ]
    ex3["frames"][1]["custom_signals_observed"] = [
        {
            "name": "first_fvg_after_array_tap",
            "candle_i_range": [0, 0],
            "measured": {
                "body": None,
                "range": None,
                "lower_wick": None,
                "upper_wick": None,
            },
            "presenter_calls_it": "ST / early entry FVG creating the intermediate term low",
        }
    ]
    remap_walkthrough(ex3, [None, None, 2, 4], log)
    log.add("custom_signals_observed", "ex3_eurgbp_market_maker_buy_model: fvg_magnet_rocket -> htf_opposing_pd_array and st_entry -> first_fvg_after_array_tap")
