from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "swing_trading_lost_art")

    set_field(
        s,
        "summary",
        "After a monthly, weekly, or daily higher-timeframe PD-array tap, use the mapped lower timeframe to trade the first same-direction FVG that develops into a new short-term range and overlapping entry zone. Once the original trade is risk-free or partially realized, the same overlap logic can be reused one rung higher for a re-entry toward the higher-timeframe objective.",
        log,
    )

    s["timeframes"]["bias_tf"] = "1d|1w|1mo"
    s["timeframes"]["setup_tf"] = "1h|4h"
    s["timeframes"]["entry_tf"] = "1h|4h"
    s["timeframes"]["strict"] = False
    s["timeframes"]["notes"] = (
        "Fractal mapping: 1mo context uses 4h for the original entry, then 1d for the re-entry; 1w and 1d contexts use 1h for the original entry, then 4h for the re-entry. The re-entry is the same overlap logic one contract-ladder rung above the original execution timeframe."
    )
    log.add("timeframes", "swing_trading_lost_art: timeframe labels normalized to Traidle enums; strict=false because the entry/re-entry pair depends on the chosen higher-timeframe context")

    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    s["requires_context"]["day_of_week"] = None
    log.add("requires_context", "swing_trading_lost_art: prose higher-timeframe context -> custom_signals.htf_pd_array_tap; Monday-Wednesday preference kept as a filter instead of a hard context gate")

    s["glossary"] = {
        "Original Entry": "The first execution on the mapped lower timeframe after the higher-timeframe PD-array tap.",
        "Re-entry Opportunity": "The same overlap-based execution taken one ladder rung higher after the original trade is already risk-free or partially realized.",
    }

    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "bias_tf", "03:26"),
        step(2, "fvg.bullish", "equals", True, "setup_tf", "03:32"),
        step(3, "custom_signals.short_term_range", "equals", True, "setup_tf", "04:10"),
    ]
    log.add("signals_required", "swing_trading_lost_art: custom_signals.htf_array_tap -> registry htf_pd_array_tap; raw new_short_term_range -> registry short_term_range; overlap kept in the entry rule because breaker/prior-high partners are only partly expressible")

    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "03:26"),
        ctx.cs("short_term_range", "04:10"),
        ctx.cs("overlapping_pd_array", "10:15"),
        ctx.cs("intermediate_term_low", "04:42"),
    ]
    log.add("custom_signals", "swing_trading_lost_art: htf_array_tap -> htf_pd_array_tap, new_short_term_range -> short_term_range, overlapping_entry_zone -> overlapping_pd_array")

    s["sequence"] = [
        "Wait for price to tap the monthly, weekly, or daily higher-timeframe discount array that points to the opposing premium objective.",
        "On the mapped execution chart, mark the first bullish fair value gap that forms after the tap.",
        "Let that move become a new short-term range and buy the overlapping PD-array retracement; once the original trade is risk-free, repeat the same overlap logic one rung higher for the re-entry.",
    ]

    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None, "ny_dow >= 1 AND ny_dow <= 3", None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the overlapping part of the new short-term range, where the execution-timeframe FVG shares price with another PD array.",
        structured={"type": "limit", "reference": "custom_signals.overlapping_pd_array"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Use 1:2 RR as the training benchmark; once experienced, hold toward the higher-timeframe context high/low.",
        structured={"type": "rr_multiple", "reference": 2.0},
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the intermediate-term low of the original entry; covering just the bodies is permissible.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )

    resolve_refs(s, {}, log)

    set_field(
        s,
        "notes",
        "Fractal continuation model. The machine form captures the original entry directly and treats the higher-timeframe re-entry as the same overlap logic repeated one ladder rung higher once the first trade is already risk-free; that account-state prerequisite is not computable from price data alone. The overlap proxy is limited to the registry's fair-value-gap plus order-block confluence, so prior-high and breaker examples remain descriptive.",
        log,
    )

    drop_uncertainty(s, "ST", "the presenter defines ST here as the first bullish fair value gap after the higher-timeframe tap", log)
    s["uncertainties"].append(
        {
            "issue": "Re-entry permission depends on the state of the original trade",
            "why": "The presenter requires the first trade to be at breakeven or partially realized before the higher-timeframe re-entry is allowed, but that portfolio state is not stored in market data.",
            "ts": "09:31",
        }
    )
    s["uncertainties"].append(
        {
            "issue": "Not every overlap partner is machine-zoned",
            "why": "The registry overlap is a fair-value-gap plus order-block proxy, while the examples also cite prior highs and breakers as valid overlap partners.",
            "ts": "10:15",
        }
    )

    for e in doc["examples"]:
        for fr in e.get("frames") or []:
            for cso in fr.get("custom_signals_observed") or []:
                if cso.get("name") == "new_short_term_range":
                    cso["name"] = "short_term_range"
                    log.add("custom_alias", f"{e['example_id']}: custom_signals_observed 'new_short_term_range' -> canonical 'short_term_range'")
