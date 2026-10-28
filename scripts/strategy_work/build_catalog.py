import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from cat_part1 import TF_LABELS, TF_MINUTES, LADDER, ENUMS
from cat_part2 import build_signals, COLLECTIONS, VIEWS

DETECTIONS = {
    "fvg": "fvg.formed", "fvg.bullish": "fvg.bullish == true", "fvg.bearish": "fvg.bearish == true",
    "fvg.mitigated": "fvg.mitigated == true", "fvg.filled": "fvg.filled == true", "fvg.inverted": "fvg.inverted == true",
    "balanced_range": "balanced_range.formed",
    "order_block": "order_block.formed", "order_block.bullish": "order_block.bullish == true",
    "order_block.bearish": "order_block.bearish == true", "order_block.standard": "order_block.subtype == standard",
    "order_block.breaker": "order_block.subtype == breaker", "order_block.rejection": "order_block.subtype == rejection",
    "order_block.propulsion": "order_block.subtype == propulsion", "order_block.mitigated": "order_block.mitigated == true",
    "order_block.invalidated": "order_block.invalidated == true",
    "structural_event": "any structural_event at t",
    "structural_event.bullish_bos": "structural_event.kind == bullish_bos",
    "structural_event.bearish_bos": "structural_event.kind == bearish_bos",
    "structural_event.bullish_choch": "structural_event.kind == bullish_choch",
    "structural_event.bearish_choch": "structural_event.kind == bearish_choch",
    "structural_event.mss": "structural_event.is_mss == true",
    "structural_event.liquidity_sweep_high": "structural_event.kind == liquidity_sweep_high",
    "structural_event.liquidity_sweep_low": "structural_event.kind == liquidity_sweep_low",
    "swing_high": "swing_high.formed", "swing_low": "swing_low.formed",
    "swing_high.swept": "swing_high.swept == true", "swing_low.swept": "swing_low.swept == true",
    "equal_highs": "equal_level.is_high == true", "equal_lows": "equal_level.is_high == false",
    "pdh": "pdh", "pdl": "pdl", "premium_discount": "premium_discount.*", "kill_zone": "kill_zone", "bias": "bias",
}

MIRROR = {
    "bullish": "bearish", "high": "low", "highs": "lows", "highest": "lowest", "max": "min", "gt": "lt", "gte": "lte",
    ">": "<", ">=": "<=", "crosses_above": "crosses_below", "crosses_above_signal": "crosses_below_signal",
    "gt_signal": "lt_signal", "gte_signal": "lte_signal",
    "swing_high": "swing_low", "gap_high": "gap_low", "zone_high": "zone_low", "refined_high": "refined_low",
    "range_high": "range_low", "candle_high": "candle_low", "upper_wick": "lower_wick",
    "liquidity_sweep_high": "liquidity_sweep_low", "sweep_high": "sweep_low", "bullish_bos": "bearish_bos",
    "bullish_choch": "bearish_choch", "bullish_fvg": "bearish_fvg", "bullish_ob": "bearish_ob",
    "pdh": "pdl", "premium": "discount", "long": "short", "buy": "sell", "bsl": "ssl",
    "intermediate_term_high": "intermediate_term_low", "short_term_high": "short_term_low", "ith": "itl", "sth": "stl",
    "equal_highs": "equal_lows", "is_high": "is_low",
}

LEGACY = {
    "swing_point.is_high": "swing_high.formed (true) / swing_low.formed (false)",
    "swing_point": "swing_high / swing_low", "swing_points": "swing_high + swing_low",
    "fvg (equals formed)": "fvg.formed", "fvg_mitigation": "fvg.mitigated", "bearish_fvg": "fvg.bearish",
    "bullish_fvg": "fvg.bullish", "order_blocks": "order_block", "liquidity_sweep_low": "structural_event.liquidity_sweep_low",
    "liquidity_sweep_high": "structural_event.liquidity_sweep_high", "bullish_choch": "structural_event.bullish_choch",
    "bearish_choch": "structural_event.bearish_choch",
    "order_block.mitigation": "custom_signals.mitigation_block (engine never assigns subtype mitigation)",
    "order_block.invalidation": "order_block.invalidated",
    "candle.high (equals true)": "custom signal (invalid boolean use of a price field)",
    "price (equals <custom>)": "custom_signals.<name>",
    "structural_event.kind == st_formation": "custom_signals.<name> (custom concept inside an engine enum is invalid)",
    "bias == established": "custom_signals.<name> (not an engine bias value)",
    "fvg.gap_extreme": "fvg.gap_low (bullish) / fvg.gap_high (bearish)",
    "sweep_extreme": "structural_event.candle_low (bullish sweep of a low) / structural_event.candle_high",
    "sweeping_candle.high": "structural_event.candle_high", "sweeping_candle.low": "structural_event.candle_low",
    "liquidity_levels": "swing_high[0].price@<tf> / equal_level.price_b (target levels)",
}

EXTERNAL = {
    "external.news_event": "a scheduled high-impact news release (CPI/FOMC/NFP) on the NY day of t; needs a calendar feed",
    "external.news_minute": "NY minute-of-day of that release",
    "external.correlated": "the same engine fields computed on another symbol (e.g. external.correlated.swing_low[0].price); one process = one symbol",
    "external.dxy": "US dollar index candles (e.g. external.dxy.close)",
    "external.bonds": "30-year treasury futures candles",
    "external.equity_session": "cash-equity session state (open/closed); crypto perps trade 24/7",
}

FUNCTIONS = {
    "abs": {"args": ["x"], "returns": "number"},
    "min": {"args": ["x", "y", "..."], "returns": "number"},
    "max": {"args": ["x", "y", "..."], "returns": "number"},
    "avg": {"args": ["x", "n"], "returns": "number", "window": "x evaluated at t, t-1, .., t-n+1; n evaluated at t"},
    "highest": {"args": ["x", "n"], "returns": "number", "window": "same as avg"},
    "lowest": {"args": ["x", "n"], "returns": "number", "window": "same as avg"},
    "count": {"args": ["cond", "n"], "returns": "int", "window": "same as avg"},
    "any": {"args": ["cond", "n"], "returns": "bool", "window": "same as avg"},
    "all": {"args": ["cond", "n"], "returns": "bool", "window": "same as avg"},
}


def pdh_validity():
    v = {}
    for tf in TF_LABELS:
        m = TF_MINUTES[tf]
        if m is not None and m <= 60:
            v[tf] = "valid: candle boundaries align with NY midnight under both EST and EDT"
        elif tf in ("2h", "4h"):
            v[tf] = "valid during EDT only; during EST the 23:00 NY candle (spanning midnight) is bucketed into the previous NY day (1h misalignment)"
        elif tf in ("6h", "8h", "12h"):
            v[tf] = "misaligned: buckets span roughly 01/02, 03/04 or 07/08 NY to the next day; do not use"
        else:
            v[tf] = "degenerate: every candle starts a new NY-day bucket, so pdh/pdl = previous candle high/low (UTC-aligned)"
    return v


def build():
    signals = build_signals()
    views = {}
    for view, base in VIEWS.items():
        views[view] = {"base": base, "filter": ("bullish == true" if view.startswith("bullish") else "bullish == false"),
                       "fields": sorted(COLLECTIONS[base]["fields"].keys()), "usable_in_steps": False}
    price_refs = sorted([p for p, s in signals.items() if s["price_reference"]])
    cat = {
        "catalog_version": "1.0.0",
        "canonical_side": "bullish/long; bearish/short is obtained with the mirror table",
        "timeframes": {
            "labels": TF_LABELS, "minutes": TF_MINUTES, "roles": ENUMS["timeframe_role"], "ladder": LADDER,
            "ladder_rule": "ladder_up(tf, n): move n rungs up LADDER; a label not on the ladder first maps to the next higher rung",
            "binance_alignment": "all candles are Binance UTC-aligned; 1d/3d/1w/1mo open at 00:00 UTC, not NY midnight",
            "pdh_pdl_validity": pdh_validity(),
            "label_note": "the wire 'timeframe' string is the user's raw input (e.g. '1mo' or '1M'); strategies use the labels above",
        },
        "enums": ENUMS,
        "collections": {k: {kk: vv for kk, vv in v.items() if kk != "fields"} | {"fields": sorted(v["fields"].keys())} for k, v in COLLECTIONS.items()},
        "views": views,
        "signals": signals,
        "price_references": price_refs,
        "detections": DETECTIONS,
        "operators": {
            "step_keys": ENUMS["step_operator"],
            "step_value_rules": {
                "equals": "scalar in the signal domain", "in": "non-empty array of domain values",
                "gt|gte|lt|lte": "number", "between": "[low, high] numbers", "crosses_above|crosses_below": "number",
                "*_signal": "catalog path or custom_signals.<name>; between_signal takes [path, path]"},
            "expression_binary": ["+", "-", "*", "/", "==", "!=", "<", "<=", ">", ">=", "AND", "OR", "NOT"],
            "expression_functions": FUNCTIONS,
        },
        "mirror": MIRROR,
        "legacy_to_canonical": LEGACY,
        "external_namespace": EXTERNAL,
    }
    os.makedirs("strategies/catalog", exist_ok=True)
    with open("strategies/catalog/signal_catalog.json", "w", encoding="utf-8") as f:
        json.dump(cat, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("signals", len(signals), "price_refs", len(price_refs))


if __name__ == "__main__":
    build()
