from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "advanced_market_structure")

    log.add("category_kept", f"{s['strategy_id']}: worker proposed 'context_framework'; original category '{s['category']}' kept (valid entry category, entry rule present)")
    set_field(
        s,
        "summary",
        "Use the ITH/ITL hierarchy to decide which short-term highs stay protected while price travels toward the opposing ITL. Once that ITL is swept, expect a deeper retracement into the premium array on the timeframe immediately above the execution chart.",
        log,
    )

    s["timeframes"] = {
        "bias_tf": None,
        "setup_tf": None,
        "entry_tf": None,
        "strict": False,
        "notes": "Fractal hierarchy. setup_tf is the active structure leg, bias_tf is the timeframe immediately above it via the contract ladder, and entry_tf is the current execution chart; the walkthrough shows daily -> 1h -> 15m -> 1m.",
    }
    log.add("timeframes", "advanced_market_structure: timeframe roles grounded by the contract ladder instead of free-form Daily/1H/15m/1m prose")

    s["requires_context"]["other"] = ["custom_signals.htf_opposing_pd_array"]
    log.add("requires_context", "advanced_market_structure: prose premium-array context -> custom_signals.htf_opposing_pd_array")

    s["glossary"] = {
        "Draw on liquidity": "The immediate target level price is moving toward; in this lesson it is the opposing intermediate-term low during the bearish push, then the higher-timeframe premium array once that low is swept."
    }
    log.add("glossary", "advanced_market_structure: registry-covered STH/STL/ITH/ITL entries removed; only draw on liquidity remains as presenter jargon")

    s["signals_required"] = [
        step(1, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "07:24"),
        step(2, "candle.high", "gte_signal", "custom_signals.htf_opposing_pd_array", "entry_tf", "15:38"),
    ]
    log.add("signals_required", "advanced_market_structure: raw bearish liquidity-sweep premise kept as step 1; higher-timeframe premium-array retracement grounded as candle.high gte_signal custom_signals.htf_opposing_pd_array")

    s["custom_signals"] = [
        ctx.cs("intermediate_term_high", "03:45"),
        ctx.cs("intermediate_term_low", "03:26"),
        ctx.cs("htf_opposing_pd_array", "15:38"),
        ctx.cs("overlapping_pd_array", "18:48"),
    ]
    log.add("custom_signals", "advanced_market_structure: local ITH/ITL/STH definitions replaced by canonical registry copies; short_term_high removed because it stays descriptive and never becomes a rule input")

    s["sequence"] = [
        "Mark the active bearish leg with its protected intermediate-term high and its opposing intermediate-term low draw on liquidity.",
        "As price pushes toward that ITL, treat the short-term highs inside the leg as protected unless the larger structure fails first.",
        "When the ITL is swept, expect a deeper retracement rather than immediate continuation.",
        "Project that retracement into the premium array on the timeframe immediately above the execution chart.",
        "Prefer the retracement when multiple PD arrays overlap inside that higher-timeframe premium zone.",
    ]

    set_numeric(
        s["invalidation"],
        ["close > custom_signals.intermediate_term_high AND NOT swing_low[1].swept"],
    )
    set_numeric(s["filters"], ["custom_signals.overlapping_pd_array"])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="After the intermediate-term low is swept, look for the larger retracement into the premium array on the timeframe immediately above the execution chart.",
        structured={"type": "limit", "reference": "custom_signals.htf_opposing_pd_array"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing intermediate-term low.",
        structured={"type": "target_level", "reference": "custom_signals.intermediate_term_low"},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop loss beyond the protected intermediate-term high.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_high", "buffer_atr": None},
    )
    log.add("rules", "advanced_market_structure: htf_premium_array -> custom_signals.htf_opposing_pd_array; intermediate-term rule references normalized to custom_signals.*")

    s["notes"] = (
        "Framework, not a standalone execution model: it explains when a completed draw on liquidity should lead to a deeper pullback into the next-higher-timeframe PD array. "
        "The presenter’s structure is a swing hierarchy (ITH/ITL and protected short-term highs), not Traidle’s BOS/CHoCH bias state machine. "
        "The walkthrough is taught on the bearish side even though the idea is symmetric."
    )

    e = example(doc, "fractal_ms_daily_to_1m")
    remap_walkthrough(e, [None, None, None, 2], log)
    e["engine_check"]["may_differ"][0]["signal"] = "custom_signals.intermediate_term_high"
    log.add("engine_check", "fractal_ms_daily_to_1m: may_differ swing_point -> custom_signals.intermediate_term_high because the lesson uses a multi-swing ITH/ITL hierarchy rather than raw swing-point detections")
