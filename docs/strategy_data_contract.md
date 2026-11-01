# Traidle strategy data contract

Version 1.0.0. This document fixes what a strategy file may reference, so that rule evaluation, historical
success ratios and the LLM reasoning layer read the same numbers. The engine code is the source of truth;
every statement cites `file:symbol`. Machine-readable twins: `strategies/catalog/signal_catalog.json`
(closed vocabulary), `strategies/catalog/custom_signals.json` (custom signal registry) and
`strategies/catalog/constants.json` (tunable `K_*` constants).

## 1. What Traidle stores for a symbol

One process streams one USD-M futures symbol (`src/binance.cpp:main`). The user picks any subset of the 15
supported timeframes at start-up; each selected timeframe runs in its own thread with its own
`IndicatorEngine` and `SmcEngine` (`src/binance.cpp:runTimeframeStream`). There is no cross-timeframe and no
cross-symbol state. A strategy that needs a timeframe needs that timeframe to be selected.

### 1.1 Source 1: per-timeframe CSV of closed candles

Path `<symbol lowercase>/<english name>.csv`, e.g. `bnbusdt/fifteen_minutes.csv`
(`src/binance.cpp:englishNameForInterval`). Rows are appended only for closed candles
(`src/historical_get.cpp:ensureContinuousHistory` skips rows whose `closeTime + 2000 ms` is in the future;
live rows are written by `src/binance.cpp:writeClosedCandleRow`). 23 columns
(`src/historical_get.cpp:kTotalColumns`, header in `openCsvAppend`):

| # | column | type | unit | known at | warm-up / notes |
|---|---|---|---|---|---|
| 0 | timestamp | `YYYY-MM-DD HH:MM:SS` | UTC, candle **open** time | close of the candle | `formatUtcTimestamp(candle.openTime)` |
| 1 | base_volume | number | base asset | close | expression name `volume` |
| 2 | quote_volume | number | quote asset (USDT) | close | `quote_volume` |
| 3 | taker_buy_base_volume | number | base asset | close | `taker_buy_volume` |
| 4 | taker_buy_quote_volume | number | quote asset | close | `taker_buy_quote_volume` |
| 5 | sma | number | price | close | SMA(20) of close; blank until the 20th candle |
| 6 | ema | number | price | close | EMA(20), SMA-seeded; blank until the 20th candle |
| 7 | macd_line | number | price | close | EMA(12)-EMA(26); blank until the 26th candle |
| 8 | macd_signal | number | price | close | EMA(9) of macd_line; blank until the 34th candle |
| 9 | macd_histogram | number | price | close | line - signal; blank until the 34th candle |
| 10 | rsi | number | 0-100 | close | RSI(14), Wilder; 100 when avg loss is 0; blank until the 15th candle |
| 11 | stoch_k | number | 0-100 | close | %K(14); 50 when the 14-candle range is 0; blank until the 14th candle |
| 12 | stoch_d | number | 0-100 | close | SMA(3) of %K; blank until the 16th candle |
| 13 | bb_middle | number | price | close | = sma |
| 14 | bb_upper | number | price | close | middle + 2 x population stddev(20) |
| 15 | bb_lower | number | price | close | middle - 2 x population stddev(20) |
| 16 | atr | number | price | close | ATR(14), Wilder; first TR = high-low; blank until the 14th candle |
| 17 | vwap | number | price | close | typical-price VWAP weighted by base volume, reset at 00:00 **UTC**; no warm-up; on 1d and above every candle is a new UTC day so vwap = (h+l+c)/3 |
| 18 | obv | number | base asset | close | cumulative from the first stored candle (starts at 0); the level depends on where history starts, only changes are meaningful |
| 19-22 | open, high, low, close | number | price | close | |

Indicator definitions: `src/indicators.h:IndicatorSnapshot`, `src/indicators.cpp:IndicatorEngine::update`.
Blank indicator cells are null. History: backfill starts `kHistoryWindowMs` = 5 x 365 days before "now"
(`src/historical_get.cpp:kHistoryWindowMs`), or at the symbol's listing if younger; the file is never trimmed,
so it grows past 5 years. Several comments still say 3 years (`src/historical_get.h` header,
`src/binance.cpp:runTimeframeStream`); the constant says 5.

Integrity caveats observed in the local data (see the report): files created by older builds keep their old
header and old-schema rows (5 or 9 columns) followed by 23-column rows; the replay silently skips non-23-column
rows (`src/historical_get.cpp:replayExistingRowsIntoEngine`). Duplicate closed-candle rows with the same
timestamp exist at the tail of `xauusdt/fifteen_minutes.csv` and `xauusdt/one_hour.csv`; nothing de-duplicates
by open time. Consumers must read only 23-column rows, de-duplicate by timestamp (keep the first) and treat
the header as unreliable.

### 1.2 Source 2: in-memory SmcEngine state

Not persisted. At start-up every 23-column CSV row is replayed through `IndicatorEngine` and collected
(`src/binance.cpp:runTimeframeStream` seedHistory callback), then `SmcEngine::replayHistory` runs one sequential
pass (`src/smc-ict/smc_engine.cpp`). Afterwards `SmcEngine::update` runs once per closed candle with that
candle's ATR. The engine reads only open time and OHLC (`src/historical_get.cpp:replayExistingRowsIntoEngine`
reconstructs only those plus base volume). Active lists are bounded and pruned every 1,000 candles to the
last 50,000 candles (`src/smc-ict/smc_engine.h:kViewWindowCandles`, `kPruneIntervalCandles`).

Wire names (`src/visualization.cpp:*ToJson`) are the canonical field names. Objects:

| object (wire) | fields | retention | known at |
|---|---|---|---|
| swing_high / swing_low (`swing_highs`, `swing_lows`, `swings`) | price, open_time, swept, swept_at | 128 per side (`SwingTracker::kMaxRetained`) | close of the candle after the pivot (`swings.cpp:SwingTracker::update`, strict `>`/`<`) |
| fvg (`fvgs`, `fvgs_formed`, `fvgs_changed`) | bullish, gap_low, gap_high, ce, formed_at, displacement_at, confirmed_at, mitigated, filled, inverted, mitigated_at, inverted_at | 512 (`FvgTracker::kMaxActive`) | close of C3; flags at the close of the changing candle |
| balanced_range (`balanced_ranges`) | range_low, range_high, formed_at | pruned at 50,000 | close of the second FVG's C3 |
| order_block (`order_blocks`, `obs_formed`, `obs_changed`) | bullish, subtype, zone_low, zone_high, refined_low, refined_high, mean_threshold, anchor_at, invalidated, mitigated, invalidated_at, mitigated_at | 256 (`OrderBlockTracker::kMaxActive`) | close of the candle completing the FVG+BOS pair (standard/propulsion) or confirming the swing (rejection) |
| structural_event (`structural_events`, update only) | kind, reference_price, candle_close, open_time, is_mss | event stream only, not in the snapshot | close of the breaking/sweeping candle |
| equal_level (`equal_levels`) | is_high, price_a, price_b, open_time_a, open_time_b | 128 | close confirming the second swing |
| premium_discount | swing_low, swing_high, equilibrium, ote_618, ote_705, ote_786, bullish_leg, origin_at, terminus_at | recomputed each candle | close of t |
| pdh / pdl, previous_day_levels.day_start_at | price | latest only | close of the first candle of the new NY day |
| bias | bullish, bearish, undetermined | state | close of t |
| kill_zone | asian, london, new_york_am, london_close, none (wire null) | per candle | open time of t |

Detection rules in brief (full notes per field in the catalog):

* FVG: C1/C2/C3 window; bullish needs `C1.high < C3.low`, a bullish C2 and `body(C2) >= 1.5 x` the average body of
  the 20 candles before C2, i.e. ending at C1 (`smc_engine.cpp:SmcEngine::update` pushes each body one candle late,
  so the CHoCH->MSS test, which judges the current candle, excludes it and the candle before it). `mitigated` = a later candle range contains CE; `filled` and `inverted` are set together when
  a later close is beyond the far edge (bullish: close < gap_low) (`fair_value_gaps.cpp:FvgTracker::update`).
* Order block: on a bullish BOS or new bullish FVG, the most recent bearish candle in the last 64 is the anchor
  if a bullish FVG and a bullish BOS both occurred after it. `mean_threshold` = midpoint of the anchor's full
  high-low range. `mitigated` = any later range overlap with the zone. `invalidated` = later close beyond MT; if
  that same close is also beyond the far edge the subtype becomes `breaker`. Rejection blocks come from every
  confirmed swing whose wick (body to extreme) is >= 0.33 of the range; propulsion = a new standard OB nested
  inside an already-mitigated same-direction OB. `mitigation` exists in the enum but is never assigned
  (`order_blocks.cpp`).
* BOS: close beyond the most recent same-side swing while bias is not opposite; at most one BOS per candle in
  practice. CHoCH: close beyond the protected swing set at the last BOS; flips bias. MSS = CHoCH candle with
  body >= 1.5 x average and a same-direction FVG confirmed on that candle (`break_of_structure.cpp`,
  `change_of_character.cpp`, `smc_engine.cpp`).
* Liquidity sweep: **any** later candle with `high > swing.price` (or `low <`) on an unswept retained swing,
  wick or body, marks it swept and emits `liquidity_sweep_*`; a BOS can fire on the same candle
  (`liquidity.cpp:LiquidityTracker::update`). A pure sweep (turtle soup) is therefore the sweep event AND
  `candle_close` back on the original side of `reference_price` (registry `pure_sweep`).
* Equal levels: a newly confirmed swing within `0.1 x ATR(14)` of the previous same-side swing (fallback
  0.05% of price while ATR warms up).
* Premium/discount: the most recent swing high and swing low; `bullish_leg` when the high confirmed later;
  OTE levels measured back from the terminus.
* PDH/PDL: NY-calendar-day high/low of **this timeframe's** candles, bucketed by open time
  (`previous_levels.cpp`).
* Engine bias is the BOS/CHoCH structure state of that timeframe only. It is not the presenters' bias (draw on
  liquidity, FVG respect, PD-array context), which is always a custom signal.

## 2. Timeframes

Labels: `1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w 1mo` (`src/candlesticks.cpp:isSupportedBinanceInterval`;
`1mo` is the Binance `1M`). Strategy roles: `bias_tf`, `setup_tf`, `entry_tf`; a role holds `null` or labels
joined by `|` with no spaces (alternatives, any one of which satisfies the role).

Ladder (for relative rules such as "the timeframe above"): `1m -> 5m -> 15m -> 1h -> 4h -> 1d -> 1w -> 1mo`.
`ladder_up(tf, n)` moves `n` rungs up; a label not on the ladder first maps to the next higher rung. A null
role is resolved at evaluation time: `entry_tf = setup_tf`, `setup_tf = entry_tf`, `bias_tf =
ladder_up(setup_tf, 1)`, unless the strategy's `timeframes.notes` states a different resolution.

All candles are Binance UTC-aligned. 1d/3d/1w/1mo open at 00:00 UTC (19:00 or 20:00 New York), never at NY
midnight. Validity of NY-day concepts per timeframe:

| timeframe | pdh/pdl (engine) | NY midnight/weekly open from this TF | kill_zone |
|---|---|---|---|
| 1m-1h | valid | valid | valid (open-time classification) |
| 2h, 4h | valid in EDT only; in EST the 23:00 NY candle spanning midnight lands in the previous day | approximate | coarse: a 4h candle opening 00:00 NY is `none` though it covers London |
| 6h, 8h, 12h | misaligned (buckets start 01-08 NY) | invalid | coarse |
| 1d, 3d, 1w, 1mo | degenerate: every candle is its own bucket, so pdh/pdl = previous candle's UTC high/low | invalid | meaningless |

## 3. Time conventions

* `open_time` / CSV `timestamp`: UTC candle **open** time, epoch ms in expressions.
* New York local time uses the fixed US rule (second Sunday of March 02:00 EST to first Sunday of November
  02:00 EDT) in `src/smc-ict/ny_time.cpp:isEasternDaylightTime`; nothing reads the host time zone.
* `ny_minute`: minute of the NY day (0-1439) of the candle's open time (`ny_time.cpp:minuteOfNewYorkDay`).
  `ny_dow`: ISO day of week of the open time in NY, 1 = monday ... 7 = sunday (same EST/EDT rule).
* A candle belongs to a window `[start, end)` when `start <= ny_minute < end`, exactly like
  `kill_zones.cpp:classifyKillZone`. Engine kill zones (fixed): asian 20:00-24:00, london 02:00-05:00,
  new_york_am 07:00-10:00, london_close 10:00-12:00. Presenter windows (e.g. 08:30-11:00, 13:30-16:00, the 09:30
  open) are custom `time_rule` signals over `ny_minute` (or `requires_context.time_window_ny`), never new kill
  zones.
* Midnight/weekly/session opens and session highs/lows are not stored; they are derived from lower-timeframe
  candles with `ny_minute`/`ny_dow` (registry: `midnight_open`, `weekly_open`, `london_open_price`,
  `asian_range_high/low`). Valid on 1m-1h only. On the one DST-change day per season the NY day has 23 or 25
  hours and offsets computed from `ny_minute` are off by one hour for that day.
* `requires_context.day_of_week` uses lowercase full English names.

## 4. Temporal semantics (no look-ahead)

Everything is evaluated **as of the close of candle t** on a timeframe, using only candles closed by then.
Cross-timeframe references (`x@4h`) read the most recent candle of that timeframe whose close time is at or
before the close of t. Engine objects become knowable at their `known_at` (section 1.2), never earlier: a swing
one candle after the pivot, an FVG at C3's close, an OB when its FVG+BOS pair completes.

Signal kinds: **event** (true only on the candle where it happens: `fvg.formed`, `structural_event.kind`),
**state** (a value carried from candle to candle: `bias`, `fvg.mitigated`), **level** (a price or time).

### 4.1 signals_required

* Steps are ordered. Step k is satisfied at the first candle close, on the step's timeframe, at or after the
  close at which step k-1 became known, where the condition holds. `max_candles_after_previous_step` (integer,
  counted on step k's timeframe from that candle; 0 = the same candle) bounds the wait; null = unbounded.
  `optional: true` steps may be skipped.
* Collections (`fvg`, `order_block`, `swing_high`, `swing_low`, `structural_event`, `equal_level`,
  `balanced_range`): a step on an **identity** field (`formed`, `bullish`, `bearish`, `kind`, `is_high`,
  `subtype` other than `breaker`) binds a NEW instance emitted at the satisfying candle. Later steps on
  **lifecycle** fields (`mitigated`, `filled`, `inverted`, `state`, `invalidated`, `swept`,
  `subtype == breaker`) and **attribute** fields (prices, times) refer to the most recently bound instance of
  that collection and are satisfied at the candle where the flag becomes true.
* A custom signal used as a step is evaluated at the step's candle; its value (for `level` kinds) is frozen at
  that candle and reused by later `*_signal` comparisons and by rule references.
* Operators: `equals`, `in`, `gt`, `gte`, `lt`, `lte`, `between` ([low, high]), `crosses_above`,
  `crosses_below` (value at t-1 on one side, at t on the other), and `*_signal` variants whose value is another
  catalog path or `custom_signals.<name>`. Values must lie in the signal's domain (catalog `enums`).
  Custom concepts are never placed inside an engine enum; OR/AND groups live inside one named custom signal.
* `traidle_supported` is true only for catalog engine signals.

### 4.2 Rule references

`entry_rule/target_rule/stop_rule.structured = {type, reference, buffer_atr?}`; `type` in
`limit | stop | beyond | level | target_level | rr_multiple`. `reference` is a catalog price reference
(optionally indexed / scoped, e.g. `swing_high[0].price@bias_tf`; unindexed collection references mean the bound
instance) or `custom_signals.<name>`; for `rr_multiple` it is a number. `buffer_atr` is an ATR(14) multiple of
the entry timeframe: a number, a `K_*` name, or null. When the presenter gave no rule, text, structured and
provenance are all null.

## 5. Not available

Not stored and not derivable from the two sources: other symbols (SMT, DXY, bonds, correlations), economic
calendar, cash-equity session state, order book / funding / open interest, tick data inside a candle (so the
order of high and low inside one candle is unknown), true volume profile. These live in the `external.*`
namespace and make a strategy tier C (`market:needs_external_symbols`, `market:needs_news_calendar`,
`market:needs_equity_session`).

Not produced by the engine but derivable (custom signals, tier B): the intermediate-term high/low hierarchy
(`SwingTracker::intermediateTermHigh/Low` exists but is not exposed), protected highs/lows (kept privately in
`ChochTracker`), session ranges and opening prices, candlestick patterns, Fibonacci/swing projections, presenter
FVG touch semantics, presenter breaker/mitigation patterns, consolidation/expansion, dealing ranges.

Real volume imbalances and liquidity voids are rare on crypto perpetuals. Measured on the stored 15m files
(both sides for voids, bullish side for imbalances): a strict liquidity void (candle range fully outside the
previous range, registry `liquidity_void`) occurs in 17 of 176,728 BNBUSDT candles, 76 of 27,883 XAUUSDT and 9 of
24,887 XAGUSDT. A candle that merely opens beyond the previous range is far more common (548 BNBUSDT) and is not a
void. A body gap with overlapping wicks occurs in 12,480 BNBUSDT candles only because open differs from the
previous close by a tick, so the registry `volume_imbalance` requires the gap to exceed `K_EQUAL_LEVEL_ATR` x ATR
(engine "same price" tolerance): 2 BNBUSDT, 192 XAUUSDT, 299 XAGUSDT. TradFi perpetuals show real gaps around
market closures.

## 6. Expression language (`numeric_definition`)

Single-line, pure expression, no prose. Grammar (EBNF):

```
expr      := or
or        := and { "OR" and }
and       := not { "AND" not }
not       := "NOT" not | cmp
cmp       := sum [ ("==" | "!=" | "<" | "<=" | ">" | ">=") sum ]
sum       := term { ("+" | "-") term }
term      := unary { ("*" | "/") unary }
unary     := "-" unary | primary
primary   := NUMBER | TIME | KCONST | "(" expr ")" | func "(" expr { "," expr } ")" | ref | ENUMWORD | "true" | "false"
ref       := name [ "[" INT "]" ] { "." name } [ "[" "-" sum "]" ] [ "@" scope ]
scope     := LABEL | "bias_tf" | "setup_tf" | "entry_tf"
func      := abs | min | max | avg | highest | lowest | count | any | all
TIME      := HH ":" MM        (value = minutes since NY midnight; 24:00 allowed)
KCONST    := "K_" NAME        (must exist in constants.json)
```

Identifiers (all in the catalog):

* Candle fields of the evaluated candle: `open high low close volume quote_volume taker_buy_volume
  taker_buy_quote_volume body range upper_wick lower_wick ny_minute ny_dow open_time`.
* Indicators: `sma ema macd_line macd_signal macd_histogram rsi stoch_k stoch_d bb_middle bb_upper bb_lower atr
  vwap obv`.
* Engine state: `bias kill_zone pdh pdl premium_discount.<field> previous_day_levels.<field> tf_minutes`.
* Collections with an optional instance index `k >= 0` (0 = most recently formed as of t) and a field:
  `swing_high[k].price`, `swing_low[k]`, `fvg[k]`, `order_block[k]`, `structural_event[k]`, `equal_level[k]`,
  `balanced_range[k]`, plus direction views `bullish_fvg[k]`, `bearish_fvg[k]`, `bullish_ob[k]`,
  `bearish_ob[k]`. Without an index the reference means the instance bound by the strategy's steps; if none is
  bound, all unindexed references to one collection inside one expression bind existentially to the same
  instance (for `structural_event`: an event emitted at t), e.g. the pure sweep
  `structural_event.kind == liquidity_sweep_low AND structural_event.candle_close >= structural_event.reference_price`.
* `custom_signals.<name>`: another custom signal defined in the same strategy (its value; for `level` kinds the
  price).
* Enum words: values of `structural_event_kind`, `bias`, `kill_zone`, `order_block_subtype`, `fvg_state`.
* `external.*`: not available (tier C).

Semantics:

* `x[-n]`: the value n closed candles ago on the same timeframe; n may be an expression evaluated at t and is
  floored to an integer; `n <= 0` or beyond history gives null.
* `@scope`: evaluate on another timeframe as of the close of t (section 4).
* `avg/highest/lowest/count/any/all(x, n)`: x evaluated at t, t-1, ..., t-n+1; n evaluated at t, floored;
  n < 1 gives null. Inside the window, `ny_minute` etc. are those of each candle.
* Arithmetic is real-valued. Booleans are 1/0 in arithmetic (used to select between two prices without an
  if/else construct). Missing values are null; arithmetic or comparison with null is null; null is false in
  AND/OR/NOT and in steps.
* Time values (`open_time`, `*_at`) are epoch ms; `(open_time - T) / (tf_minutes * 60000)` is the number of
  candles since time T. `tf_minutes` is invalid on 1mo.
* Registry convention for `level` kinds: if `value_expr` is null the `numeric_definition` is the price;
  otherwise `numeric_definition` is the validity condition and `value_expr` the price (null when invalid).

## 7. Units and numeric grounding

Allowed units: ATR(14) multiples of a stated timeframe; percent of price; fractions of a leg or range (0-1);
R-multiples; candle counts on a stated timeframe; NY minute of day; candle-geometry ratios (body/range,
wick/range). Forbidden in `numeric_definition` and `structured`: pips, points, ticks, absolute prices and any
symbol-specific number; presenter-stated market-specific numbers stay verbatim in text/quotes. Presenter-stated
numbers (R:R, candle counts, Fibonacci levels, percentages, clock times) are used exactly. Any other number is a
named `K_*` constant in `constants.json` with a rationale, sweep range and `definition_source: inferred`;
engine constants are reused for identical concepts (displacement 1.5 x / 20-candle average, equal-level
0.1 x ATR, BPR 10 candles, rejection wick 0.33, OB lookback 64). Discretionary rules get
`numeric_definition: null` and a line in the report's not-computable table.

## 8. Canonical side and mirror

Symmetric strategies and all registry definitions are written for the **bullish / long** side. The bearish
side is obtained by applying the catalog `mirror` table token-wise (bullish<->bearish, high<->low,
swing_high<->swing_low, gap_high<->gap_low, zone_high<->zone_low, liquidity_sweep_high<->liquidity_sweep_low,
pdh<->pdl, highest<->lowest, max<->min, gt<->lt, gte<->lte, crosses_above<->crosses_below, premium<->discount,
...), flipping the sign of additive offsets applied to mirrored prices (`low - K x atr` <-> `high + K x atr`).
Example narratives and candles are never mirrored: they describe what the presenter showed.

## 9. Strategy file shape

Every file is `{"strategies": [...], "examples": [...]}`; `strategy.examples` lists example ids. Closed value
sets live in the catalog `enums` (category, direction, bias, session, timeframe labels, example_role,
trade outcome, fvg state, kill_zone, provenance, definition_source, custom signal kind, structured type,
market tags, chart time zone, precision). Allowed schema exceptions: the envelope, the optional
`numeric_definition` on `invalidation[]`/`filters[]` items, and the closed value sets. Placeholder strings
(`"null"`, `"N/A"`, `""`) are JSON null. `ts` is `MM:SS` (`HH:MM:SS` only for videos longer than an hour).
`tools/strategy_lint` enforces all of this.

## 10. Verification of the stated facts

| fact | verdict | evidence / correction |
|---|---|---|
| F1 CSV schema, closed only, 23 cols | confirmed | `historical_get.cpp:kTotalColumns`, `openCsvAppend`; correction: history window is 5 years (`kHistoryWindowMs`), comments say 3; only user-selected timeframes are streamed; legacy-schema and duplicate rows exist in local files |
| F2 SMC state rebuilt by replay | confirmed | `binance.cpp:runTimeframeStream` -> `SmcEngine::replayHistory`; pruning every 1,000 candles to 50,000 |
| F3 no repainting | confirmed with nuances | swings confirm one candle late; FVG at C3; OB on FVG+BOS completion; PDH/PDL only at the close of the first candle of the new NY day (one candle later than the day boundary) |
| F4 sweeps fire on any breach | confirmed | `liquidity.cpp` checks `high > price` / `low < price`; contradicts `smc_types.h` comments ("wick-only") |
| F5 engine bias != presenter bias | confirmed | `break_of_structure.cpp`, `change_of_character.cpp` |
| F6 premium_discount = latest swing pair + OTE | confirmed | `premium_discount.cpp:compute` |
| F7 OB stricter; breaker/rejection/propulsion; mitigation unused | confirmed, one correction | MT is the midpoint of the full high-low range, not of the bodies (`order_blocks.cpp:formFromAnchor`); presenters' MT of bodies is custom |
| F8 FVG displacement, CE mitigation, far-edge fill | confirmed | multiplier 1.5 over the 20 bodies before C2; the MSS test lags one more candle (`smc_engine.cpp`); `inverted` is set with `filled` although `smc_types.h` says "near edge" |
| F9 PDH/PDL per timeframe | confirmed | valid 1m-1h; 2h/4h EDT only; 6h-12h misaligned; >= 1d degenerate (section 2) |
| F10 kill zones fixed, DST-aware | confirmed | `kill_zones.cpp:kZones`; classified by open time only |
| F11 UTC-aligned HTF candles, UTC VWAP | confirmed | `indicators.cpp` VWAP day = `openTime / 86400000` |
| F12 not produced by engine | confirmed | ITH/ITL helper exists but is unexposed; strict liquidity voids and tolerance-filtered volume imbalances are rare on crypto data, real on TradFi perps (section 5) |
| F13 engine constants | confirmed | `kDisplacementMultiplier` 1.5, rolling 20, `kEqualLevelAtrMultiplier` 0.1, `kBprRecencyWindowCandles` 10, `kMinRejectionWickFraction` 0.33, `kLookbackCandles` 64 |
