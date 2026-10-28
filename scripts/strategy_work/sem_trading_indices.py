from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "indices_session_sequence")
    set_field(
        s,
        "summary",
        "On indices, the daily sequence flips between AM and PM expansion depending on whether the higher-timeframe objective is reached before 08:30 New York. Trade the designated expanding session with a sweep-and-fair-value-gap entry while treating the presenter's AM and PM windows as custom equity-session time blocks.",
        log,
    )

    s["timeframes"] = {
        "bias_tf": "4h|1d",
        "setup_tf": "5m|15m",
        "entry_tf": "1m",
        "strict": False,
        "notes": "bias_tf names the higher-timeframe objective (commonly 4h or 1d). setup_tf provides the session-sequence context, while entry_tf is the preferred 1m execution chart for indices; some examples align a 5m FVG with the 1m entry."
    }
    log.add("timeframes", "indices_session_sequence: '4h|daily' -> '4h|1d'; setup_tf reordered to '5m|15m'")

    s["requires_context"] = {
        "bias": [],
        "session": ["london"],
        "time_window_ny": None,
        "day_of_week": None,
        "other": [
            "custom_signals.htf_pd_array_tap",
            "custom_signals.objective_reached",
            "custom_signals.arjoio_ny_am_session",
            "custom_signals.arjoio_ny_pm_session",
            "market:needs_equity_session",
        ],
    }
    log.add("requires_context", "indices_session_sequence: presenter AM/PM windows moved to registry time_rule signals; HTF draw/objective kept as custom context instead of an engine bias/session enum")

    s["glossary"] = {
        "Opening Range": "The 09:30-10:30 New York period that often sets the high or low of the day; see custom_signals.equities_open_judas for the opening sweep proxy.",
        "Closing Hour": "The 15:00-16:00 New York period ('power hour') when volatility often accelerates into the day's target or profit-taking."
    }
    log.add("glossary", "indices_session_sequence: registry-backed AM/PM session terms removed; kept only non-registry presenter jargon")

    s["signals_required"] = [
        step(1, "structural_event.kind", "equals", "liquidity_sweep_low", "entry_tf", "05:12"),
        step(2, "custom_signals.pure_sweep", "equals", True, "entry_tf", "05:12", maxc=0),
        step(3, "fvg.bullish", "equals", True, "entry_tf", "08:15"),
    ]
    log.add("mirror", "indices_session_sequence: normalized on the canonical bullish side; bearish PM examples rely on the catalog mirror table")
    log.add("signals_required", "indices_session_sequence: executable trigger rewritten as sweep -> pure_sweep -> bullish FVG; AM/PM branch selection stays in context via objective_reached and custom session windows")

    s["custom_signals"] = [
        ctx.cs("arjoio_ny_am_session", "01:00"),
        ctx.cs("arjoio_ny_pm_session", "01:17"),
        ctx.cs("htf_pd_array_tap", "02:00"),
        ctx.cs("objective_reached", "09:10"),
        ctx.cs("equities_open_judas", "05:37"),
        ctx.cs("pure_sweep", "05:12"),
    ]

    s["sequence"] = [
        "Choose the expanding session from the sequence: use AM when the higher-timeframe objective is still unreached before 08:30 New York, otherwise wait for PM after the AM consolidation.",
        "In that expanding session, wait for sell-side liquidity to be swept.",
        "Require the sweeping candle to close back above the swept level, then buy the bullish FVG that forms after it.",
    ]

    set_numeric(s["invalidation"], ["custom_signals.objective_reached"])
    set_numeric(s["filters"], ["custom_signals.equities_open_judas", None])
    log.add("filters", "indices_session_sequence: opening-range preference -> equities_open_judas; instrument preference kept text-only because symbol scope is not available in the contract")

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the bullish fair value gap that forms after the session liquidity sweep in the designated expanding session.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the higher-timeframe objective identified before the session sequence begins.",
        structured=None,
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the sweep extreme.",
        structured={"type": "beyond", "reference": "structural_event.candle_low", "buffer_atr": None},
    )
    log.add("rules", "indices_session_sequence: entry/target kept as text (exact FVG edge and HTF target type are not always stated); stop sweep_extreme -> structural_event.candle_low on the canonical long side")

    s["management"]["notes"] = "Be cautious in the closing hour; it can accelerate into the target or turn into profit-taking after the objective is already reached."

    s["notes"] = (
        "This is a session-sequence framework first and an entry trigger second: London determines whether AM expands or only engineers liquidity for PM. "
        "The presenter’s 08:30-11:00 and 13:30-16:00 windows are custom equity-session rules, not engine kill zones, and objective_reached is proxied by a pre-08:30 higher-timeframe FVG/order-block tap rather than every possible discretionary target."
    )

    drop_uncertainty(s, "Specific entry parameters for the 'ST model'", "this rewrite keeps only the stated sweep-and-FVG trigger, not the full mentorship model", log)
    s["uncertainties"].append({
        "issue": "objective_reached only proxies engine FVG/order-block targets",
        "why": "The presenter also names swing objectives such as intermediate-term highs/lows, but the reused registry entry only detects a pre-08:30 tap of the latest higher-timeframe engine FVG or order block.",
        "ts": "09:10",
    })

    remap_walkthrough(example(doc, "ex1_nq_consolidation_expansion"), [None, 1, None, None, 3], log)
    remap_walkthrough(example(doc, "ex2_es_expansion_consolidation"), [None, None, 1, 3], log)
    remap_walkthrough(example(doc, "ex3_es_am_fake_move"), [None, 1, None, None, None], log)
