from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "gap_and_imbalance_overlap_confluence")
    set_category(s, "context_framework", log)
    set_field(s, "summary",
              "Treat fair value gaps, balanced price ranges, liquidity voids, and volume imbalances as inefficiency-based points of interest where price may rebalance before continuing. The highest-probability subset is an imbalance overlapping another PD array, but this episode does not define a full entry-stop-target model.",
              log)
    s["timeframes"]["notes"] = ("Fractal across timeframes. The presenter also points to news shocks and TradFi-style session closures as sources of "
                                "liquidity voids and body gaps, but on crypto perpetual data those patterns are rare.")
    s["requires_context"]["other"] = ["custom_signals.overlapping_pd_array"]
    log.add("requires_context", "gap_and_imbalance_overlap_confluence: overlap preference prose -> custom_signals.overlapping_pd_array (supported FVG+OB subset)")
    s["glossary"] = {
        "BISI": "Institutional label for a bullish fair value gap; the engine detects the displacement subset as fvg.bullish, while the presenter also accepts custom_signals.fvg_without_displacement.",
        "SIBI": "Institutional label for a bearish fair value gap; mirror the bullish-side definitions.",
        "BPR": "Balanced price range: the overlap of opposing fair value gaps; Traidle's closest built-in is balanced_range, but it adds a 10-candle recency rule not stated here.",
    }
    s["signals_required"] = []
    s["custom_signals"] = [
        ctx.cs("fvg_without_displacement", "03:48"),
        ctx.cs("overlapping_pd_array", "10:34"),
    ]
    s["sequence"] = []
    log.add("sequence", "gap_and_imbalance_overlap_confluence: conceptual masterclass -> no executable step chain; procedure moved into notes")
    set_numeric(s["invalidation"], ["NOT custom_signals.fvg_without_displacement", None])
    set_numeric(s["filters"], ["custom_signals.fvg_without_displacement", None])
    s["entry_rule"] = keep_rule(s["entry_rule"], structured=None)
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = null_rule(s["stop_rule"])
    log.add("rules", "gap_and_imbalance_overlap_confluence: target_rule and stop_rule placeholders -> null; entry_rule kept as conceptual text only")
    s["management"]["notes"] = None
    log.add("placeholder_null", "gap_and_imbalance_overlap_confluence.management.notes conceptual placeholder -> null")
    resolve_refs(s, {
        "ref:order_block": "order_block_mitigation",
        "ref:breaker_block": "breaker_block_setup",
        "ref:mitigation_block": "mitigation_block_setup",
    }, log)
    s["notes"] = ("Context framework, not a finished execution model. The presenter treats any wick-gap three-candle pattern as a valid FVG, so "
                  "custom_signals.fvg_without_displacement captures that geometry while engine fvg.* remains the displacement-only subset. "
                  "custom_signals.overlapping_pd_array is the closest supported overlap proxy, but breaker-block, mitigation-block, liquidity-void, "
                  "volume-imbalance, and presenter-style BPR confluence still remain conceptual or detector backlog.")

    e1 = example(doc, "ex1_fvg_identification")
    remap_walkthrough(e1, [None, None, None], log)
    e2 = example(doc, "ex2_bisi_sibi_identification")
    remap_walkthrough(e2, [None, None, None], log)
    e2["frames"][0]["signals_observed"]["fvgs"][1]["presenter_calls_it"] = "SIBI / sell-side imbalance"
    log.add("asr", "ex2_bisi_sibi_identification: 'SIBI / cell site imbalance' -> 'SIBI / sell-side imbalance'")
    log.add("walkthrough_map", "gap_and_imbalance_overlap_confluence: walkthrough step mappings cleared because signals_required is empty")
