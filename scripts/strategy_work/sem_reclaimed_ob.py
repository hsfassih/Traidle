from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "reclaimed_order_block_overlap")
    set_field(s, "summary", "Mark the bullish order blocks formed during the sell curve into a higher-timeframe discount array. After price shifts into the buy curve and reclaims one from below, only trade the retest when a new right-curve PD array overlaps that block.", log)
    s["timeframes"]["bias_tf"] = "4h"
    s["timeframes"]["setup_tf"] = "15m"
    s["timeframes"]["entry_tf"] = "15m"
    s["timeframes"]["notes"] = "The transcript explicitly uses 4h context and 15m execution. The higher-timeframe array lives on bias_tf; the order block, reclaim, overlap, and entry all run on 15m."
    log.add("timeframes", "reclaimed_order_block_overlap: bias_tf/setup_tf/entry_tf htf/ltf/ltf -> 4h/15m/15m from the presenter's named example")
    s["requires_context"]["other"] = []
    log.add("requires_context", "reclaimed_order_block_overlap: prose market-maker-model requirement moved into step2 custom_signals.market_maker_curve_shift and notes")
    s["glossary"] = {
        "Market maker model": "A sell curve into a higher-timeframe discount array followed by a buy curve; here proxied by custom_signals.market_maker_curve_shift.",
        "Reclaimed order block": "A bullish order block from the sell curve that price later closes back above on the buy curve; see custom_signals.reclaimed_trade_through.",
    }
    s["signals_required"] = [
        step(1, "order_block.bullish", "equals", True, "setup_tf", "02:49"),
        step(2, "custom_signals.market_maker_curve_shift", "equals", True, "setup_tf", "03:20"),
        step(3, "custom_signals.reclaimed_trade_through", "equals", True, "setup_tf", "04:01"),
        step(4, "custom_signals.reclaimed_pd_array_overlap", "equals", True, "setup_tf", "08:14"),
    ]
    log.add("signals_required", "reclaimed_order_block_overlap: steps reordered to the presenter's chronology (left-curve bullish OB -> curve shift -> reclaim above the block -> overlap filter)")
    log.add("signals_required", "reclaimed_order_block_overlap: overlapping_pd_array replaced with reclaimed_pd_array_overlap so breaker/mitigation partners stay in-scope instead of narrowing the meaning to FVG+OB only")
    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "03:20"),
        ctx.cs("market_maker_curve_shift", "03:20"),
        ctx.cs("reclaimed_trade_through", "04:01"),
        ctx.cs("breaker_pattern", "10:30"),
        ctx.cs("swing_failure_pattern", "10:30"),
        ctx.cs("reclaimed_pd_array_overlap", "08:14"),
    ]
    log.add("custom_signals", "reclaimed_order_block_overlap: added registry-backed market_maker_curve_shift, reclaimed_trade_through, breaker_pattern, swing_failure_pattern, and reclaimed_pd_array_overlap")
    s["sequence"] = [
        "Mark the bullish order block formed during the sell curve into the higher-timeframe discount array.",
        "Wait for the higher-timeframe array tap and bullish structure shift that starts the buy curve.",
        "Wait for price to close back above that left-curve bullish order block so it becomes reclaimed support.",
        "Only keep the setup if a new right-curve PD array overlaps the reclaimed block, then buy the retest of that reclaimed block.",
    ]
    s["invalidation"] = []
    log.add("invalidation", "reclaimed_order_block_overlap: placeholder invalidation item 'Not explicitly defined in this video.' removed (no presenter content; same rule as rule placeholders)")
    set_numeric(s["filters"], ["custom_signals.reclaimed_pd_array_overlap"])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Buy the retracement into the reclaimed bullish order block once a new right-curve PD array overlaps it.",
        structured={"type": "limit", "reference": "order_block.zone_high"},
        prov="inferred",
    )
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "reclaimed_order_block_overlap: target_rule and stop_rule placeholders ('Not explicitly defined...') -> null; entry structured at the reclaimed OB near edge because the transcript names the overlap zone but not a finer edge")
    s["notes"] = (
        "Written on the canonical bullish side; the bearish examples are the mirror. The market maker model is proxied as a 4h array tap plus a bullish CHoCH/BOS on 15m, because the presenter names the curve logic but does not define a separate detector for the full two-curve profile. "
        "The overlap filter keeps right-curve confluence with a new FVG, a later order block, a breaker level, or a swing-failure-pattern mitigation level."
    )
    s["uncertainties"].append({
        "issue": "Exact entry edge inside the overlap cluster",
        "why": "The presenter requires the reclaimed block to overlap another PD array but does not name a finer edge inside that cluster; the structured rule uses the reclaimed order block's near edge on the bullish side.",
        "ts": "14:35",
    })

    e1 = example(doc, "ex1_market_maker_curves_schematic")
    remap_walkthrough(e1, [1, 2, 3], log)

    e2 = example(doc, "ex2_bearish_reclaimed_ob_clutter")
    remap_walkthrough(e2, [2, 1, 3, None], log)

    e3 = example(doc, "ex3_bearish_reclaimed_ob_overlap")
    remap_walkthrough(e3, [3, 3, 4, None], log)
    for cso in e3["frames"][0]["custom_signals_observed"]:
        if cso.get("name") == "overlapping_pd_array":
            cso["name"] = "reclaimed_pd_array_overlap"
            log.add("custom_alias", "ex3_bearish_reclaimed_ob_overlap: custom_signals_observed 'overlapping_pd_array' -> 'reclaimed_pd_array_overlap'")

    e4 = example(doc, "ex4_bullish_reclaimed_ob_overlap")
    remap_walkthrough(e4, [1, 3, 4, None], log)
    for cso in e4["frames"][0]["custom_signals_observed"]:
        if cso.get("name") == "overlapping_pd_array":
            cso["name"] = "reclaimed_pd_array_overlap"
            log.add("custom_alias", "ex4_bullish_reclaimed_ob_overlap: custom_signals_observed 'overlapping_pd_array' -> 'reclaimed_pd_array_overlap'")
