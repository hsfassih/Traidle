from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "order_block_mitigation")
    set_field(s, "summary", "With the prevailing bias, a down candle (or run of down candles) whose high is then broken is a bullish order block; enter when price returns to it. The block holds while no candle closes beyond 50% of its bodies.", log)
    s["timeframes"]["notes"] = "No timeframes given in this video; fractal. Steps run on setup_tf, resolved at evaluation time (contract: timeframe ladder)."
    s["requires_context"]["other"] = []
    log.add("requires_context", "order_block_mitigation: prose 'Must align with the current directional bias (e.g., following a liquidity sweep)' moved to notes (presenter bias, not computable)")
    s["glossary"] = {
        "Mean threshold": "50% of the order block candle bodies, ignoring wicks; see custom_signals.mean_threshold_bodies.",
        "Order block (bullish)": "A down candle (or run of consecutive down candles) coming off support whose high gets broken; see custom_signals.presenter_order_block.",
    }
    s["signals_required"] = [
        step(1, "custom_signals.presenter_order_block", "equals", True, "setup_tf", "03:41"),
        step(2, "candle.low", "lte_signal", "custom_signals.ob_fvg_overlap_sensitivity", "setup_tf", "03:59"),
    ]
    log.add("signals_required", "step1 order_block.bullish (engine OB needs FVG+BOS; presenter does not) -> custom_signals.presenter_order_block")
    log.add("signals_required", "step2 lte_signal order_block.zone_high -> custom_signals.ob_fvg_overlap_sensitivity (wick top vs body open rule)")
    s["custom_signals"] = [ctx.cs("presenter_order_block", "03:41"), ctx.cs("mean_threshold_bodies", "04:16"),
                           ctx.cs("ob_fvg_overlap_sensitivity", "06:48")]
    s["sequence"] = [
        "Mark a down candle (or run of down candles) off support whose high is then broken: the bullish order block.",
        "Buy when price returns to the block: at the wick top if the wick overlaps an FVG, otherwise at the body open.",
    ]
    set_numeric(s["invalidation"], ["close < custom_signals.mean_threshold_bodies"])
    set_numeric(s["filters"], [None, None])
    s["entry_rule"] = keep_rule(s["entry_rule"], structured={"type": "limit", "reference": "custom_signals.ob_fvg_overlap_sensitivity"})
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "order_block_mitigation: target_rule and stop_rule placeholders ('Not explicitly defined...') -> null")
    s["notes"] = ("The presenter's order block is looser than the engine's: it needs no FVG or BOS, only a broken high; the engine OB "
                  "(order_block.*) is a stricter subset. His mean threshold is 50% of the bodies, not the engine's full-range midpoint "
                  "(order_block.mean_threshold). The required bias is the presenter's directional bias (a liquidity sweep is given as "
                  "one example of how it is set), not the engine structure bias; it is not computable from this video.")
    s["uncertainties"].append({"issue": "'Coming off support' is not encoded",
                               "why": "The presenter requires the block to bounce off prior support (an FVG or rejection block in the examples) but gives no rule for which support counts.",
                               "ts": "08:04"})


