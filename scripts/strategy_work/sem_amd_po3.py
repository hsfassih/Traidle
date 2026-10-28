from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "amd_power_of_3")
    log.add("category_kept", f"{s['strategy_id']}: worker proposed 'context_framework'; original category '{s['category']}' kept (valid entry category, entry rule present)")
    set_field(
        s,
        "summary",
        "Track the active session, daily, weekly, or monthly open, then expect an opening manipulation away from the real draw on liquidity before distribution begins. The re-entry area is the symmetric accumulation band projected from the manipulation extreme back through that active open.",
        log,
    )
    set_field(
        s,
        "timeframes",
        {
            "bias_tf": None,
            "setup_tf": None,
            "entry_tf": None,
            "strict": False,
            "notes": "Fractal framework. setup_tf is the active AMD profile chart (session, daily, weekly, or monthly), bias_tf is the higher profile that supplies the draw on liquidity, and entry_tf is whichever lower chart the trader later uses for execution.",
        },
        log,
        "timeframes",
    )
    s["requires_context"] = {
        "bias": [],
        "session": [],
        "time_window_ny": None,
        "day_of_week": None,
        "other": [
            "custom_signals.london_open_price",
            "custom_signals.midnight_open",
            "custom_signals.weekly_open",
            "custom_signals.monthly_open",
        ],
    }
    log.add(
        "requires_context",
        "amd_power_of_3: free-text opening anchors/context -> custom_signals.london_open_price, midnight_open, weekly_open, monthly_open",
    )
    s["glossary"] = {
        "Accumulation phase": "The symmetric re-entry band between the active AMD open and the mirrored -1 Fibonacci projection from the manipulation extreme; its exact bounds depend on whether the anchor is custom_signals.london_open_price, custom_signals.midnight_open, custom_signals.weekly_open, or custom_signals.monthly_open.",
        "Manipulation": "The opening fake move that runs liquidity away from the true distribution leg.",
        "Distribution": "The expansion leg that carries price from the open/manipulation complex into the higher-timeframe draw on liquidity.",
        "OHLC / OLHC": "The bearish / bullish candle footprint of one AMD cycle.",
    }
    log.add(
        "glossary",
        "amd_power_of_3: opening_price/accumulation_phase_range moved out of raw custom_signals and preserved as glossary/context concepts",
    )
    s["signals_required"] = [
        step(1, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "05:02"),
    ]
    s["custom_signals"] = [
        ctx.cs("london_open_price", "01:43"),
        ctx.cs("midnight_open", "11:00"),
        ctx.cs("weekly_open", "13:39"),
        ctx.cs("monthly_open", "13:39"),
    ]
    log.add(
        "custom_signals",
        "amd_power_of_3: opening_price -> london_open_price/midnight_open/weekly_open/monthly_open; accumulation_phase_range left as a glossary-only, profile-dependent concept",
    )
    s["sequence"] = [
        "Pick the active profile open and the higher-timeframe draw on liquidity.",
        "Let price manipulate through that open and run the opposite-side liquidity.",
        "Project the symmetric accumulation band from the manipulation extreme back to the active open.",
        "Use the manipulation or accumulation zone as the participation area before distribution reaches the higher-timeframe target.",
    ]
    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None, None])
    s["invalidation"][0]["text"] = "For bullish setups, stand aside once price has traded completely above the symmetric accumulation band."
    log.add(
        "mirror",
        "amd_power_of_3: short-side invalidation rewritten on the canonical bullish side; bearish examples remain unmirrored",
    )
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Participate during the manipulation leg or inside the accumulation band, keeping bullish entries below the active open.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing liquidity pool or higher-timeframe PD array that the active profile is drawing toward.",
        structured=None,
    )
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "amd_power_of_3: placeholder stop_rule -> null; entry/target stay textual because the active opening anchor is profile-dependent")
    s["notes"] = (
        "Framework, not a complete execution model. The required bias is the presenter's higher-timeframe draw on liquidity / premium-discount read, not Traidle's BOS/CHoCH structure bias. "
        "The active anchor can be the London open, New York midnight open, weekly open, or monthly open; monthly_open stays not computable in the expression grammar, and the symmetric accumulation band therefore remains descriptive rather than machine-exact."
    )
    s["uncertainties"].append(
        {
            "issue": "Active AMD anchor stays profile-dependent",
            "why": "The lesson switches among London, daily, weekly, and monthly profiles without a deterministic rule for which anchor is active at one evaluation point, so the accumulation band cannot be reduced to one exact numeric expression.",
            "ts": "01:43",
        }
    )

    e4 = example(doc, "ex4_daily_ohlc_amd")
    daily_sig = e4["frames"][0]["custom_signals_observed"][0]
    if daily_sig.get("name") != "midnight_open":
        log.add("custom_alias", "ex4_daily_ohlc_amd: custom_signals_observed 'opening_price' -> canonical 'midnight_open'")
        daily_sig["name"] = "midnight_open"

    e5 = example(doc, "ex5_monthly_weekly_amd")
    weekly_sig = dict(e5["frames"][0]["custom_signals_observed"][0])
    monthly_sig = dict(weekly_sig)
    weekly_sig["name"] = "weekly_open"
    weekly_sig["presenter_calls_it"] = "Weekly open"
    monthly_sig["name"] = "monthly_open"
    monthly_sig["presenter_calls_it"] = "Monthly open"
    e5["frames"][0]["custom_signals_observed"] = [weekly_sig, monthly_sig]
    log.add(
        "custom_alias",
        "ex5_monthly_weekly_amd: mixed opening_price observation split into canonical weekly_open and monthly_open",
    )
