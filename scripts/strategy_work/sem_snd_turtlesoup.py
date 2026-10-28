from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "seek_and_destroy_turtle_soup")
    set_field(
        s,
        "summary",
        "In seek-and-destroy consolidation, buy only from the discount-discount edge of the dealing range after a sell-side sweep closes back above the swept low and an ST model forms. Take the conservative target near range equilibrium because these conditions are mean-reverting.",
        log,
    )
    s["timeframes"]["bias_tf"] = "1d"
    s["timeframes"]["notes"] = "Daily anchors the dealing range; setup and entry execute on 5m. Written on the canonical bullish side, while premium-premium shorts are the mirror."
    log.add("timeframes", "seek_and_destroy_turtle_soup: bias_tf 'Daily' -> '1d'; notes clarified the daily range anchor and bullish canonical rewrite")
    s["requires_context"]["other"] = ["custom_signals.dealing_range"]
    log.add("requires_context", "seek_and_destroy_turtle_soup: prose Seek-and-Destroy condition -> custom_signals.dealing_range")
    s["glossary"] = {
        "Discount discount": "The bullish outer quarter of the dealing range where longs are sought; see custom_signals.quadrant_extreme.",
        "Premium premium": "The bearish mirror of discount discount: the upper quarter of the dealing range where shorts are sought.",
    }
    s["signals_required"] = [
        step(1, "custom_signals.quadrant_extreme", "equals", True, "setup_tf", "02:17"),
        step(2, "structural_event.kind", "equals", "liquidity_sweep_low", "setup_tf", "03:52"),
        step(3, "custom_signals.pure_sweep", "equals", True, "setup_tf", "03:52", maxc=0),
        step(4, "custom_signals.st_model", "equals", True, "setup_tf", "04:16"),
    ]
    log.add("mirror", "seek_and_destroy_turtle_soup: generalized premium-premium / discount-discount narration rewritten on the canonical bullish side; bearish setups are mirror cases")
    log.add("signals_required", "seek_and_destroy_turtle_soup: step2 liquidity_sweep_high -> liquidity_sweep_low and step3 pure_sweep added because turtle soup requires the sweep candle to close back on the original side")
    s["custom_signals"] = [
        ctx.cs("dealing_range", None),
        ctx.cs("dealing_range_high", None),
        ctx.cs("dealing_range_low", None),
        ctx.cs("quadrant_extreme", "02:17"),
        ctx.cs("pure_sweep", "03:52"),
        ctx.cs("st_model", "04:16"),
        ctx.cs("intermediate_term_low", "09:22"),
    ]
    s["sequence"] = [
        "Confirm that price is trading inside a dealing range.",
        "Wait for price to reach the discount-discount edge of that range.",
        "Require a sell-side sweep that closes back above the swept low.",
        "Buy the ST model retracement and target nearby equilibrium.",
    ]
    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None, None])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Enter on the retracement into the second bullish fair value gap / overlapping PD array left by the ST model.",
        structured={"type": "limit", "reference": "fvg.gap_high"},
        prov="inferred",
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the nearest opposing liquidity pool or fair value gap around the 50% equilibrium of the dealing range.",
        structured=None,
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop beyond the intermediate-term low and give it room to breathe.",
        structured={"type": "beyond", "reference": "custom_signals.intermediate_term_low", "buffer_atr": None},
    )
    log.add("rules", "seek_and_destroy_turtle_soup: entry anchored to the ST model retracement FVG, target kept text-only because the presenter means the current dealing-range midpoint rather than engine premium_discount.equilibrium, stop anchored to intermediate_term_low")
    resolve_refs(s, {}, log)
    s["notes"] = (
        "Mean-reversion framework, not a trend-following model. The range condition is proxied by custom_signals.dealing_range and the turtle soup itself "
        "by custom_signals.pure_sweep. Written on the bullish side; the first example is the bearish premium-premium mirror. The presenter says the target "
        "should stay close because price often returns to the entry in consolidation."
    )
    s["uncertainties"].append({
        "issue": "How a broken range becomes a new valid dealing range",
        "why": "The presenter says to wait for failure to follow through after a break, but gives no exact threshold for when the old range is abandoned and the new one is confirmed.",
        "ts": "08:33",
    })

    e1 = example(doc, "ex1_seek_destroy_premium")
    remap_walkthrough(e1, [1, 2, 4, None, None], log)
    e2 = example(doc, "ex2_seek_destroy_discount")
    remap_walkthrough(e2, [None, 1, None, 4, None, None], log)

    s = strategy(doc, "trending_turtle_soup_strength")
    set_field(
        s,
        "summary",
        "With a higher-timeframe draw on liquidity in view, wait for a lower-timeframe sell-side sweep into the setup PD array during London or New York AM. Enter on strength above the sweep candle and hold toward the opposing 4h swing.",
        log,
    )
    s["timeframes"]["notes"] = "4h supplies the directional draw on liquidity, the 1h setup chart carries the array context, and 5m supplies the sweep-and-stop-order trigger. Written on the canonical bullish side."
    log.add("timeframes", "trending_turtle_soup_strength: notes clarified the 4h draw, 1h setup context, 5m execution, and bullish canonical side")
    s["requires_context"]["other"] = ["custom_signals.htf_pd_array_tap"]
    log.add("requires_context", "trending_turtle_soup_strength: prose HTF-array requirement -> custom_signals.htf_pd_array_tap")
    s["signals_required"] = [
        step(1, "custom_signals.htf_pd_array_tap", "equals", True, "setup_tf", "13:10"),
        step(2, "structural_event.kind", "equals", "liquidity_sweep_low", "entry_tf", "14:21"),
        step(3, "custom_signals.pure_sweep", "equals", True, "entry_tf", "14:21", maxc=0),
    ]
    log.add("signals_required", "trending_turtle_soup_strength: fvg.state=open -> custom htf_pd_array_tap, killzone_timing removed in favor of requires_context.session, and pure_sweep replaces the undefined sweep_candle concept")
    s["custom_signals"] = [
        ctx.cs("htf_pd_array_tap", "13:10"),
        ctx.cs("pure_sweep", "14:21"),
    ]
    s["sequence"] = [
        "Keep the 4h draw on liquidity in view and wait for price to trade into the setup PD array.",
        "During London or New York AM, require a sell-side sweep on 5m that closes back above the swept low.",
        "Place a buy stop above the sweep candle and hold for the opposing 4h swing high.",
    ]
    set_numeric(s["invalidation"], [None])
    set_numeric(s["filters"], [None, None])
    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="Place a buy stop above the high of the candle responsible for sweeping the sell-side liquidity low.",
        structured={"type": "stop", "reference": "structural_event.candle_high"},
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="Target the opposing 4h liquidity objective, typically the swing high above price.",
        structured={"type": "target_level", "reference": "swing_high[0].price@bias_tf"},
        prov="inferred",
    )
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="Place the stop below the sweep candle low with enough room to breathe; the quoted pip/point sizes are market-specific examples only.",
        structured={"type": "beyond", "reference": "structural_event.candle_low", "buffer_atr": "K_STOP_BUFFER_ATR"},
        prov="inferred",
    )
    log.add("rules", "trending_turtle_soup_strength: sweep_candle.* / liquidity_levels / sweep_extreme replaced by structural_event.candle_high, swing_high[0].price@bias_tf, and structural_event.candle_low + K_STOP_BUFFER_ATR")
    s["notes"] = (
        "Trend-following turtle soup. The session timing is expressed with requires_context.session because the presenter names only the standard London and New York AM kill zones. "
        "The turtle soup itself is custom_signals.pure_sweep: the sweep must close back above the swept low. The market-specific pip and point numbers remain in the text and quote; "
        "the machine stop uses the sweep-candle extreme plus K_STOP_BUFFER_ATR as a structural proxy for 'room to breathe'."
    )
