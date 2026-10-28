from cat_part1 import SRC, ENUMS

KNOWN_CANDLE = "close of candle t (the candle itself)"


def sig(kind, typ, source, known_at, unit=None, domain=None, role="scalar", derived=None, step=True,
        expr=True, price_ref=False, warmup=None, notes=None):
    d = {"kind": kind, "type": typ, "domain": domain, "role": role, "unit": unit, "known_at": known_at,
         "source": source, "derived": derived, "usable_in_steps": step, "usable_in_expressions": expr,
         "price_reference": price_ref}
    if warmup:
        d["warmup"] = warmup
    if notes:
        d["notes"] = notes
    return d


CANDLE_FIELDS = {
    "open": ("number", "price", None, True, None),
    "high": ("number", "price", None, True, None),
    "low": ("number", "price", None, True, None),
    "close": ("number", "price", None, True, None),
    "volume": ("number", "base asset units", "CSV column base_volume", False, None),
    "quote_volume": ("number", "quote asset units (e.g. USDT)", "CSV column quote_volume", False, None),
    "taker_buy_volume": ("number", "base asset units", "CSV column taker_buy_base_volume", False, None),
    "taker_buy_quote_volume": ("number", "quote asset units", "CSV column taker_buy_quote_volume", False, None),
    "body": ("number", "price", "abs(close - open)", False, None),
    "range": ("number", "price", "high - low", False, None),
    "upper_wick": ("number", "price", "high - max(open, close)", False, None),
    "lower_wick": ("number", "price", "min(open, close) - low", False, None),
    "ny_minute": ("int", "minute of New York local day 0..1439 of the candle OPEN time (DST-aware)",
                  "smc::minuteOfNewYorkDay(open_time)", False, None),
    "ny_dow": ("int", "ISO day of week of the candle OPEN time in New York: 1=monday .. 7=sunday",
               "derived from open_time with the same EST/EDT rule as src/smc-ict/ny_time.cpp", False, None),
    "open_time": ("time_ms", "epoch milliseconds UTC (CSV column timestamp, candle OPEN time)", None, False, None),
}

INDICATORS = {
    "sma": ("SMA(20) of close", "first value on the 20th candle"),
    "ema": ("EMA(20) of close, SMA-seeded", "first value on the 20th candle"),
    "macd_line": ("EMA(12)-EMA(26) of close", "first value on the 26th candle"),
    "macd_signal": ("EMA(9) of macd_line", "first value on the 34th candle"),
    "macd_histogram": ("macd_line - macd_signal", "first value on the 34th candle"),
    "rsi": ("RSI(14), Wilder smoothing; 100 when avg loss is 0", "first value on the 15th candle"),
    "stoch_k": ("%K(14); 50 when the 14-candle range is 0", "first value on the 14th candle"),
    "stoch_d": ("SMA(3) of %K", "first value on the 16th candle"),
    "bb_middle": ("SMA(20) of close", "first value on the 20th candle"),
    "bb_upper": ("bb_middle + 2 * population stddev(20)", "first value on the 20th candle"),
    "bb_lower": ("bb_middle - 2 * population stddev(20)", "first value on the 20th candle"),
    "atr": ("ATR(14), Wilder smoothing, first TR = high-low", "first value on the 14th candle"),
    "vwap": ("typical-price VWAP weighted by base volume, reset at 00:00 UTC (candle open_time/86400000)",
             "no warm-up; degenerates to (h+l+c)/3 on 1d and above because every candle starts a new UTC day"),
    "obv": ("On-Balance Volume, cumulative from the first stored candle (starts at 0)", "no warm-up; absolute level depends on history start"),
}

COLLECTIONS = {
    "swing_high": {
        "source": SRC["swing"], "retention": "last 128 per side (SwingTracker::kMaxRetained)",
        "index": "swing_high[0] = most recently confirmed swing high as of t",
        "known_at": "close of the candle AFTER the pivot (3-candle rule, strict >): pivot open_time + 1 candle",
        "fields": {
            "formed": ("event", "bool", None, "identity", "derived: a swing high was confirmed at t", "true only at the confirming candle"),
            "price": ("level", "number", "price", "attribute", None, None),
            "open_time": ("level", "time_ms", "epoch ms", "attribute", None, "pivot candle open time"),
            "swept": ("state", "bool", None, "lifecycle", None, "set by liquidity.cpp on ANY later candle with high > price (wick or body)"),
            "swept_at": ("level", "time_ms", "epoch ms", "attribute", None, "open time of the sweeping candle"),
        }},
    "fvg": {
        "source": SRC["fvg"], "retention": "last 512 (FvgTracker::kMaxActive), pruned after 50,000 candles; filled FVGs stay flagged",
        "index": "fvg[0] = most recently formed FVG as of t; views bullish_fvg[k] / bearish_fvg[k] filter by direction",
        "known_at": "close of C3 (confirmed_at candle); lifecycle flags at the close of the candle that changes them",
        "fields": {
            "formed": ("event", "bool", None, "identity", "derived: confirmed_at == open_time of t", None),
            "bullish": ("event", "bool", None, "identity", None, "requires C2 body >= 1.5 x rolling 20-candle average body (lagged)"),
            "bearish": ("event", "bool", None, "identity", "derived: NOT bullish", None),
            "gap_low": ("level", "number", "price", "attribute", None, None),
            "gap_high": ("level", "number", "price", "attribute", None, None),
            "ce": ("level", "number", "price", "attribute", None, "(gap_low + gap_high) / 2"),
            "formed_at": ("level", "time_ms", "epoch ms", "attribute", None, "open time of C1"),
            "displacement_at": ("level", "time_ms", "epoch ms", "attribute", None, "open time of C2"),
            "confirmed_at": ("level", "time_ms", "epoch ms", "attribute", None, "open time of C3"),
            "mitigated": ("state", "bool", None, "lifecycle", None, "a later candle's range touched CE (low <= ce <= high) or the far edge was closed through"),
            "filled": ("state", "bool", None, "lifecycle", None, "a later close beyond the FAR edge (bullish: close < gap_low)"),
            "inverted": ("state", "bool", None, "lifecycle", None, "always set together with filled"),
            "mitigated_at": ("level", "time_ms", "epoch ms", "attribute", None, None),
            "inverted_at": ("level", "time_ms", "epoch ms", "attribute", None, None),
            "state": ("state", "enum", "fvg_state", "lifecycle", "derived: filled -> filled (== inverted); else mitigated -> mitigated; else open", "filled and inverted are indistinguishable"),
        }},
    "order_block": {
        "source": SRC["ob"], "retention": "last 256 (OrderBlockTracker::kMaxActive); invalidated OBs stay flagged and stop updating",
        "index": "order_block[0] = most recently formed OB as of t; views bullish_ob[k] / bearish_ob[k]",
        "known_at": "close of the candle that completes the FVG+BOS pair after the anchor (standard/propulsion) or the swing-confirming candle (rejection)",
        "fields": {
            "formed": ("event", "bool", None, "identity", "derived: sequence == t", None),
            "bullish": ("event", "bool", None, "identity", None, None),
            "bearish": ("event", "bool", None, "identity", "derived: NOT bullish", None),
            "subtype": ("event", "enum", "order_block_subtype", "identity", None, "standard|rejection|propulsion at formation; breaker is a LIFECYCLE transition (same candle as invalidated, close beyond far edge); mitigation is never assigned"),
            "zone_low": ("level", "number", "price", "attribute", None, "anchor candle low (rejection: wick zone)"),
            "zone_high": ("level", "number", "price", "attribute", None, "anchor candle high (rejection: wick zone)"),
            "refined_low": ("level", "number", "price", "attribute", None, "bullish: anchor low; bearish: anchor open"),
            "refined_high": ("level", "number", "price", "attribute", None, "bullish: anchor open; bearish: anchor high"),
            "mean_threshold": ("level", "number", "price", "attribute", None, "(zone_high + zone_low)/2 = midpoint of the FULL anchor range, not of bodies"),
            "anchor_at": ("level", "time_ms", "epoch ms", "attribute", None, None),
            "mitigated": ("state", "bool", None, "lifecycle", None, "a later candle range overlapped [zone_low, zone_high]"),
            "invalidated": ("state", "bool", None, "lifecycle", None, "a later close beyond mean_threshold (bullish: close < MT)"),
            "mitigated_at": ("level", "time_ms", "epoch ms", "attribute", None, None),
            "invalidated_at": ("level", "time_ms", "epoch ms", "attribute", None, None),
        }},
    "structural_event": {
        "source": SRC["ev"] + "; " + SRC["bos"] + "; " + SRC["choch"] + "; " + SRC["sweep"],
        "retention": "event stream only (smc_update.structural_events); NOT in smc_snapshot",
        "index": "structural_event[0] = most recent event as of t; unindexed refs inside one expression bind existentially to one event emitted at t",
        "known_at": "close of the breaking/sweeping candle t (event)",
        "fields": {
            "kind": ("event", "enum", "structural_event_kind", "identity", None, "liquidity_sweep_* fire on ANY breach of an unswept swing (high > price / low < price), wick OR body; a BOS can co-occur on the same candle"),
            "reference_price": ("level", "number", "price", "attribute", None, "the swing/protected level broken or swept"),
            "candle_close": ("level", "number", "price", "attribute", None, None),
            "open_time": ("level", "time_ms", "epoch ms", "attribute", None, None),
            "is_mss": ("event", "bool", None, "attribute", None, "CHoCH only: body(t) >= 1.5 x lagged avg body AND a same-direction FVG confirmed at t"),
            "candle_high": ("level", "number", "price", "attribute", "derived join: CSV high at open_time", "the event candle's high (e.g. the sweep extreme of a sweep high)"),
            "candle_low": ("level", "number", "price", "attribute", "derived join: CSV low at open_time", "the event candle's low"),
        }},
    "equal_level": {
        "source": SRC["eq"], "retention": "last 128",
        "index": "equal_level[0] = most recent equal-high/low pair",
        "known_at": "close of the candle confirming the second swing",
        "fields": {
            "formed": ("event", "bool", None, "identity", "derived", None),
            "is_high": ("event", "bool", None, "identity", None, "|price_b - price_a| <= 0.1 x ATR(14) (fallback 0.05% of price while ATR warms up)"),
            "price_a": ("level", "number", "price", "attribute", None, None),
            "price_b": ("level", "number", "price", "attribute", None, None),
            "open_time_a": ("level", "time_ms", "epoch ms", "attribute", None, None),
            "open_time_b": ("level", "time_ms", "epoch ms", "attribute", None, None),
        }},
    "balanced_range": {
        "source": SRC["bpr"], "retention": "pruned after 50,000 candles",
        "index": "balanced_range[0] = most recent BPR",
        "known_at": "close of the C3 of the second (overlapping) FVG; opposite FVG must be <= 10 candles older",
        "fields": {
            "formed": ("event", "bool", None, "identity", "derived", None),
            "range_low": ("level", "number", "price", "attribute", None, None),
            "range_high": ("level", "number", "price", "attribute", None, None),
            "formed_at": ("level", "time_ms", "epoch ms", "attribute", None, None),
        }},
}
COLLECTIONS["swing_low"] = dict(COLLECTIONS["swing_high"])
COLLECTIONS["swing_low"]["index"] = "swing_low[0] = most recently confirmed swing low as of t"
COLLECTIONS["swing_low"]["fields"] = dict(COLLECTIONS["swing_high"]["fields"])
COLLECTIONS["swing_low"]["fields"]["formed"] = ("event", "bool", None, "identity", "derived: a swing low was confirmed at t", None)
COLLECTIONS["swing_low"]["fields"]["swept"] = ("state", "bool", None, "lifecycle", None, "set on ANY later candle with low < price")

VIEWS = {"bullish_fvg": "fvg", "bearish_fvg": "fvg", "bullish_ob": "order_block", "bearish_ob": "order_block"}

SCALARS = {
    "bias": sig("state", "enum", SRC["bias"], KNOWN_CANDLE, domain="bias",
                notes="ENGINE structure bias (BOS/CHoCH state machine) of this timeframe only; NOT the presenters' draw-on-liquidity bias"),
    "kill_zone": sig("state", "enum", SRC["kz"], "open time of t (pure time filter)", domain="kill_zone",
                     notes="classified by candle OPEN time only; wire null -> none; asian 20:00-24:00, london 02:00-05:00, new_york_am 07:00-10:00, london_close 10:00-12:00 NY [start,end)"),
    "pdh": sig("level", "number", SRC["pdh"], "close of the first candle of the new NY day", unit="price", price_ref=True,
               notes="NY-calendar-day bucket of THIS timeframe's candles; valid only where candle boundaries align with NY midnight (see timeframes.pdh_pdl_validity)"),
    "pdl": sig("level", "number", SRC["pdh"], "close of the first candle of the new NY day", unit="price", price_ref=True,
               notes="see pdh"),
    "previous_day_levels.rollover": sig("event", "bool", SRC["pdh"], "close of the first candle of a new NY day", derived="smc_update.previous_day_levels != null"),
    "previous_day_levels.day_start_at": sig("level", "time_ms", SRC["pdh"], "rollover candle", unit="epoch ms"),
    "tf_minutes": sig("state", "int", "src/candlesticks.cpp:isSupportedBinanceInterval (kSupportedIntervals)", "static",
                      unit="minutes per candle of the evaluated timeframe", step=False,
                      notes="1mo has no fixed length; expressions using tf_minutes are invalid on 1mo"),
}
for f, unit in [("swing_low", "price"), ("swing_high", "price"), ("equilibrium", "price"), ("ote_618", "price"),
                ("ote_705", "price"), ("ote_786", "price")]:
    SCALARS["premium_discount." + f] = sig("level", "number", SRC["pd"], "close of t (recomputed every candle from the two most recent opposite swings)",
                                          unit=unit, price_ref=True)
SCALARS["premium_discount.bullish_leg"] = sig("state", "bool", SRC["pd"], "close of t",
                                              notes="true when the swing high confirmed after the swing low (retracing down); OTE measured from the terminus")
SCALARS["premium_discount.origin_at"] = sig("level", "time_ms", SRC["pd"], "close of t", unit="epoch ms")
SCALARS["premium_discount.terminus_at"] = sig("level", "time_ms", SRC["pd"], "close of t", unit="epoch ms")


def build_signals():
    out = {}
    for name, (typ, unit, derived, pref, _) in CANDLE_FIELDS.items():
        src = SRC["csv"] if derived is None or derived.startswith("CSV") else SRC["ny"] if name.startswith("ny_") else SRC["csv"]
        entry = sig("state", typ, src, KNOWN_CANDLE, unit=unit, derived=derived, price_ref=pref)
        entry["expression_alias"] = name
        out["candle." + name] = entry
    for name, (desc, warm) in INDICATORS.items():
        out[name] = sig("state", "number", SRC["ind"], KNOWN_CANDLE, unit="price" if name not in ("rsi", "stoch_k", "stoch_d", "obv", "macd_histogram") else ("0-100" if name in ("rsi", "stoch_k", "stoch_d") else "base volume" if name == "obv" else "price"),
                        warmup="blank CSV cell (null) until " + warm.split("first value on the ")[-1] if "first value" in warm else warm,
                        price_ref=name in ("sma", "ema", "bb_middle", "bb_upper", "bb_lower", "vwap"),
                        notes=desc)
    for cname, c in COLLECTIONS.items():
        for f, (kind, typ, dom, role, derived, note) in c["fields"].items():
            unit = dom if dom in ("price", "epoch ms") else None
            domain = dom if dom not in ("price", "epoch ms") else None
            out[cname + "." + f] = sig(kind, typ, c["source"], c["known_at"], unit=unit, domain=domain, role=role,
                                       derived=derived, price_ref=(typ == "number" and unit == "price"), notes=note)
    out.update(SCALARS)
    return out
