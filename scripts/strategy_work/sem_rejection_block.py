from semutil import *


def _set_example_checks(e, should_detect, may_signal, may_reason):
    e["engine_check"]["should_detect"] = should_detect
    e["engine_check"]["may_differ"] = [{"signal": may_signal, "reason": may_reason}]


def apply(doc, log, ctx):
    s = strategy(doc, "rejection_block_reversal")
    set_field(
        s,
        "summary",
        "Mark a bullish rejection block: a swing-low outsider candle with a dominant lower wick that later sees its body swept without losing the wick low. Enter the reversal away from that wick; bearish setups are the mirror.",
        log,
    )
    s["timeframes"]["notes"] = (
        "No explicit timeframe is given. The rejection block and its retest are read on the same presenter-chosen execution timeframe, "
        "so setup_tf and entry_tf resolve together at evaluation time."
    )
    log.add("timeframes", "rejection_block_reversal: no explicit timeframe -> fractal same-timeframe execution note")
    s["requires_context"]["bias"] = []
    s["requires_context"]["other"] = []
    log.add(
        "requires_context",
        "rejection_block_reversal: generic bullish/bearish bias and counter-trend prose removed from requires_context (pattern is symmetric; counter-trend emphasis kept in notes)",
    )
    s["glossary"] = {}
    log.add("glossary", "rejection_block_reversal: registry term 'rejection_block' removed from glossary")
    s["signals_required"] = [
        step(1, "custom_signals.rejection_block", "equals", True, "setup_tf", "01:17"),
        step(2, "custom_signals.rejection_block_body_sweep", "equals", True, "setup_tf", "02:21"),
    ]
    log.add(
        "signals_required",
        "rejection_block_reversal: candle.high placeholder -> custom_signals.rejection_block, then custom_signals.rejection_block_body_sweep for the body-sweep retest",
    )
    log.add(
        "mirror",
        "rejection_block_reversal: written on the canonical bullish side; bearish examples are the mirror",
    )
    s["custom_signals"] = [
        ctx.cs("rejection_block", "01:17"),
        ctx.cs("rejection_block_body_sweep", "02:21"),
    ]
    log.add(
        "custom_signals",
        "rejection_block_reversal: raw rejection_block replaced with the canonical registry entries rejection_block and rejection_block_body_sweep",
    )
    s["sequence"] = [
        "Mark the bullish rejection block at a swing low.",
        "Wait for price to sweep the body area inside its wick without taking the wick low, then buy the reversal.",
    ]
    set_numeric(s["invalidation"], ["low < order_block.zone_low"])
    set_numeric(s["filters"], ["custom_signals.rejection_block_body_sweep"])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter after price sweeps the body area inside the wick and reverses away from the rejection block.",
        structured=None,
    )
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "rejection_block_reversal: target_rule and stop_rule placeholders -> null; entry_rule kept as text-only because no exact price trigger is stated")
    s["notes"] = (
        "Written on the canonical bullish side; bearish rejection blocks are the mirror. The machine reading reuses the registry's rejection-block judgment "
        "(engine rejection order block at a swing extreme with a long wick) and adds the body-sweep retest, while the presenter's counter-trend emphasis and "
        "adjacent-candle outsider nuance remain discretionary."
    )
    s["uncertainties"].append(
        {
            "issue": "Counter-trend emphasis is not machine-tested",
            "why": "The presenter says rejection blocks often matter on counter-trend reactions, but the video gives no exact rule for measuring the larger trend context.",
            "ts": "03:41",
        }
    )
    s["uncertainties"].append(
        {
            "issue": "The final 'outsider candle' nuance is only approximated",
            "why": "The engine rejection subtype enforces a swing extreme with a dominant wick, but it does not compare neighboring candle opens/closes the way the final bearish example informally does.",
            "ts": "04:54",
        }
    )

    for eid in (
        "ex1_bearish_rejection_block",
        "ex2_bullish_rejection_block_counter_trend",
        "ex3_bullish_rejection_block_swing_low",
        "ex4_bearish_rejection_block_down_close",
    ):
        remap_walkthrough(example(doc, eid), [1, 2, None], log)

    _set_example_checks(
        example(doc, "ex1_bearish_rejection_block"),
        ["order_block.bearish", "order_block.rejection"],
        "custom_signals.rejection_block",
        "The registry maps the presenter’s outsider candle to the engine rejection-order-block subtype, which captures the swing extreme and long wick but not every discretionary body comparison.",
    )
    _set_example_checks(
        example(doc, "ex2_bullish_rejection_block_counter_trend"),
        ["order_block.bullish", "order_block.rejection"],
        "custom_signals.rejection_block_body_sweep",
        "The wick-held body sweep is machine-defined, but the presenter’s 'counter-trend respect' framing depends on discretionary higher-context reading.",
    )
    _set_example_checks(
        example(doc, "ex3_bullish_rejection_block_swing_low"),
        ["order_block.bullish", "order_block.rejection"],
        "custom_signals.rejection_block",
        "The engine match is the rejection-order-block subtype; the presenter’s language remains more visual than the stored engine state.",
    )
    _set_example_checks(
        example(doc, "ex4_bearish_rejection_block_down_close"),
        ["order_block.bearish", "order_block.rejection"],
        "custom_signals.rejection_block",
        "The last example adds an adjacent-candle open/close nuance that the engine does not encode beyond the long-wick swing-extreme rejection subtype.",
    )
    log.add(
        "examples",
        "rejection_block examples: walkthroughs remapped to the 2-step sequence and may_differ signals normalized to valid custom signal names",
    )
