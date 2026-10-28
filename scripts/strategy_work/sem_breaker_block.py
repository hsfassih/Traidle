from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "breaker_block_setup")

    set_field(
        s,
        "summary",
        "With higher-timeframe PD-array bias in place, wait for a breaker pattern that sweeps one swing extreme and then breaks back through the intervening swing on the lower timeframe. Trade the retest of that breaker block, preferably when a fair value gap overlaps it as a unicorn setup.",
        log,
    )
    set_field(s, "category", "liquidity_reversal", log, cat="category")

    tf = s["timeframes"]
    old_bias_tf = tf.get("bias_tf")
    old_setup_tf = tf.get("setup_tf")
    old_entry_tf = tf.get("entry_tf")
    tf["bias_tf"] = "4h|1d|1w|1mo"
    tf["setup_tf"] = "5m|1h|4h"
    tf["entry_tf"] = None
    tf["notes"] = (
        "Higher-timeframe bias comes from a 4h-to-monthly PD array. The breaker confirmation itself is shown fractally on 4h, 1h, or 5m, and there is no separate entry timeframe: the retest is taken on setup_tf."
    )
    log.add(
        "timeframes",
        f"breaker_block_setup: bias_tf '{old_bias_tf}' -> '{tf['bias_tf']}', setup_tf '{old_setup_tf}' -> '{tf['setup_tf']}', entry_tf {old_entry_tf!r} -> null",
    )

    s["requires_context"]["other"] = []
    log.add(
        "requires_context",
        "breaker_block_setup: prose higher-timeframe bias requirement moved to notes (presenter narrative, not a fixed computable rule).",
    )

    s["glossary"] = {
        "Unicorn setup": "A breaker block that overlaps a fair value gap; see custom_signals.unicorn_overlap.",
        "Mean threshold": "50% of the breaker candle body; see registry custom_signals.breaker_mean_threshold.",
    }

    s["signals_required"] = [
        step(1, "custom_signals.breaker_pattern", "equals", True, "setup_tf", "01:15"),
        step(2, "custom_signals.unicorn_overlap", "equals", True, "setup_tf", "05:14", optional=True, maxc=0),
    ]
    log.add(
        "signals_required",
        "breaker_block_setup: presenter breaker kept as registry breaker_pattern (sweep/failure structure), not engine order_block.breaker.",
    )
    log.add(
        "signals_required",
        "breaker_block_setup: optional FVG confluence split into custom_signals.unicorn_overlap on the breaker-confirmation candle.",
    )

    s["custom_signals"] = [
        ctx.cs("breaker_pattern", "01:15"),
        ctx.cs("unicorn_overlap", "05:14"),
    ]

    s["sequence"] = [
        "Set higher-timeframe bullish or bearish bias from a PD array narrative.",
        "On 4h, 1h, or 5m, confirm the breaker by sweeping one swing extreme and then breaking back through the intervening swing.",
        "Mark the breaker block at that breaking candle and note any overlapping fair value gap as a unicorn setup.",
        "Wait for price to retrace into the breaker block and trade in the higher-timeframe direction.",
    ]

    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None, None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter when price returns to the breaker block. Use the wick only when the breaker overlaps a fair value gap; otherwise use the body.",
        structured=None,
    )
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add(
        "rules",
        "breaker_block_setup: entry_rule kept as text with structured null because the wick-vs-body breaker level is presenter-specific and not a stable catalog reference; target/stop placeholders -> null.",
    )

    s["notes"] = (
        "The presenter's breaker pattern is the sweep/failure structure captured by registry breaker_pattern, not the engine order_block.subtype == breaker, which only appears when an existing order block later closes through its far edge. He treats a wick through the prior swing as enough to confirm the breaker, while the registry definition uses a close beyond that swing; examples flag that possible divergence. Higher-timeframe bias here is a presenter PD-array narrative, not the engine structure bias."
    )

    s["uncertainties"].append(
        {
            "issue": "Exact higher-timeframe bias source is not fixed",
            "why": "The presenter names examples like a monthly order block or a weekly premium array but does not give a closed list of valid higher-timeframe PD arrays for this setup.",
            "ts": "03:02",
        }
    )

    ex4 = example(doc, "ex4_bullish_breaker_continuation")
    may = ex4.get("engine_check", {}).get("may_differ") or []
    if may and may[0].get("signal") != "structural_event.bullish_bos":
        old = may[0].get("signal")
        may[0]["signal"] = "structural_event.bullish_bos"
        log.add(
            "legacy_signal",
            f"ex4_bullish_breaker_continuation: engine_check.may_differ '{old}' -> 'structural_event.bullish_bos'",
        )
