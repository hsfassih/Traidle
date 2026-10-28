from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "relative_strength_filtering")
    log.add("category_kept", f"{s['strategy_id']}: worker proposed 'market_condition_filter'; original category '{s['category']}' kept (valid entry category, entry rule present)")
    set_field(s, "summary", "Use the higher-timeframe bond and dollar read to decide whether a Forex or index idea is worth trading, then choose the strongest long candidate or weakest short candidate inside the correlated peer group. Execute only after the chosen chart reaches its own premium or discount array and a separate lower-timeframe confirmation model triggers.", log)

    s["timeframes"]["bias_tf"] = "1d|1w"
    s["timeframes"]["setup_tf"] = None
    s["timeframes"]["entry_tf"] = None
    s["timeframes"]["notes"] = "Daily or weekly intermarket context sets the presenter bias. The actual execution timeframe belongs to the downstream confirmation model used on the selected chart."
    log.add("timeframes", "relative_strength_filtering: HTF/Daily/Weekly -> bias_tf '1d|1w'; setup_tf and entry_tf cleared because this file only filters chart selection and delegates execution to downstream entry models")

    s["requires_context"]["other"] = [
        "custom_signals.decoupled_market",
        "custom_signals.relatively_stronger_pair",
        "market:needs_external_symbols",
    ]
    log.add("requires_context", "relative_strength_filtering: prose intermarket synchronization -> custom decoupled/relative-strength context plus market:needs_external_symbols; presenter bias is external, not engine structure bias")

    s["glossary"] = {
        "Decoupled": "A low-probability state where bonds and the dollar stop giving the normal inverse read; see custom_signals.decoupled_market.",
        "Filtering process": "The top-down pass from bonds to dollar to asset class to peer-group comparison that narrows the trade to one chart.",
        "Relative strength": "Inside a correlated peer group, prefer the chart that expands further in the trade direction or refuses to lose the structural point that the peer sweeps; see custom_signals.relatively_stronger_pair.",
    }
    log.add("glossary", "relative_strength_filtering: glossary normalized to canonical casing and linked to registry-backed decoupled/relative-strength terms")

    s["signals_required"] = []
    log.add("signals_required", "relative_strength_filtering: removed invalid bias=='established' step; this file is a market-condition filter, not a sequential single-symbol trigger")

    s["custom_signals"] = [
        ctx.cs("decoupled_market", "03:04"),
        ctx.cs("relatively_stronger_pair", "09:35"),
    ]
    log.add("custom_signals", "relative_strength_filtering: decoupled_market and relatively_stronger_pair moved to registry-backed external-symbol concepts")

    s["sequence"] = [
        "Read the daily or weekly bond chart first, then compare it with the US Dollar.",
        "If that intermarket read is decoupled, stand aside or downgrade the setup quality.",
        "Use the Dollar's higher-timeframe draw on liquidity to set the presenter bias for Forex or indices.",
        "Compare the correlated peer group and keep only the strongest long candidate or weakest short candidate.",
        "Wait for the chosen chart to reach its own premium or discount array before looking for a separate lower-timeframe confirmation model.",
    ]

    set_numeric(s["invalidation"], ["custom_signals.decoupled_market"])
    set_numeric(s["filters"], [None, None])

    s["entry_rule"] = keep_rule(
        s["entry_rule"],
        text="After choosing the strongest long candidate or weakest short candidate, wait for that chart to reach its own premium or discount array and trigger a downstream confirmation model such as SMT divergence, a breaker block, or a swing-failure pattern.",
        structured=None,
    )
    s["target_rule"] = keep_rule(
        s["target_rule"],
        text="On the chosen chart, target the opposing higher-timeframe draw on liquidity defined by that chart's own context.",
        structured=None,
    )
    s["target_rule"]["quote"] = "this already tells me a lot of what my draw on liquidity will likely be on EU then we have this liquidity pool right there"
    log.add("asr", "relative_strength_filtering.target_rule.quote: 'drone quality' -> 'draw on liquidity'")
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "relative_strength_filtering: target_rule structured htf_liquidity_pool removed (no catalog/custom level); stop_rule placeholder -> null")

    resolve_refs(s, {"ref:smt_divergence": "smt_divergence_confirmation"}, log)

    s["notes"] = "Market-condition filter, not a standalone entry model. The required bias comes from the presenter’s external bond-and-dollar read, not Traidle’s single-symbol structure bias. The selected symbol must still be executed with its own downstream confirmation setup once price reaches the chosen premium or discount array."

    remap_walkthrough(example(doc, "ex1_bonds_correlation"), [None, None], log)
    remap_walkthrough(example(doc, "ex2_forex_filtering"), [None, None, None, None], log)
    remap_walkthrough(example(doc, "ex3_indices_filtering"), [None, None, None], log)
