# Strategy Backtesting Specification

Version 1.0.0. This is the Phase 0 specification; it does not implement a
signal evaluator, simulator, or statistics engine.

## 1. Goal and architecture

Replay the strategies in `strategies/*.json` against the historical CSV data
for the expected 22 Binance and Capital.com symbols and 15 catalog timeframes.
Record every strategy occurrence and, for entry models, every resulting trade
in a deterministic ledger. A later Python stage derives statistics and
strategy weights from that ledger without an LLM.

**Stage 1 is C++.** For each market, symbol, and selected timeframe, replay
ordered closed candles through the existing `IndicatorEngine` and `SmcEngine`.
The new C++ evaluator reads the catalogued engine state and strategy rules;
the simulator then applies the entry, stop, target, time-stop, and cost rules
below and writes the ledger. Python must not reimplement signal detection.
Each timeframe has independent engine state. Cross-timeframe reads are
coordinated by candle close time, not by sharing an engine.

**Stage 2 is Python.** Read the immutable ledger and compute the statistics,
hierarchical weights, and comparisons in section 9. There is no strategy
signal detection in Stage 2.

One ledger CSV is written per `(market, symbol)` and one `run_manifest.json`
per run. The same row schema, evaluator semantics, costs, and update semantics
are used by the later live path. Historical and live rows are stored
separately and distinguished by `source`.

The repo documents 22 symbols and 15 timeframes as the goal, but the exact
22-symbol inventory, market assignments, and asset-class mapping are
**UNVERIFIED**. Do not invent them; make them explicit run inputs.

## 2. Decisions

The following decisions are fixed:

1. **Same-candle tie:** if stop and target are both reachable in one candle,
   record a loss at `-1R`, with `exit_reason=tie_loss`.
2. **Overlap:** only one active position per `(strategy, symbol, entry_tf)`,
   regardless of direction. Log a signal while that slot is occupied as
   `skipped_overlap`; this preserves raw occurrence counts separately from
   taken trades. Pending orders reserve the slot until filled, expired, or
   cancelled, so they cannot create multiple positions later.
3. **Costs:** model costs in basis points of price, separately by market and
   asset class, in [`costs.json`](../strategies/catalog/costs.json). Every
   numeric cost is marked `assumption: true` and is a placeholder for review,
   not a verified fee or spread. Binance costs include maker/taker fees per
   side and slippage per side. Capital.com candles are bid-only; model the
   round-trip spread by asset class.
4. **Direction:** strategy definitions are bullish-side canonical. For
   `direction=both`, evaluate the long side and the token-wise mirrored short
   side using `signal_catalog.json`'s `mirror` table.
5. **Presenter bias:** `requires_context.bias` is the presenter's bias, is not
   computable in v1, and is not a gate. Record both `engine_bias` and
   `bias_tf_engine_bias` for future conditional weights. Apply
   `requires_context.session` and `time_window_ny` only when computable.
6. **Tier C:** exclude Tier C strategies from v1 and list the reason in the
   manifest. Tier C includes external dependencies; the existing lint report
   also uses Tier C for dependencies that are explicitly non-computable.
7. **Engine constants:** inferred `K_*` defaults in `constants.json` remain
   unchanged for the first pass. Do not run parameter sweeps in v1.

All proposed backtest parameters are in
[`backtest_constants.json`](../strategies/catalog/backtest_constants.json).
Each has `provenance: inferred`; its default is a proposal, not an
engine-verified value.

## 3. Look-ahead and multi-timeframe rules

Evaluate a signal at the close of candle `t` using only data known at that
close, as required by section 4 of
[`strategy_data_contract.md`](strategy_data_contract.md). Engine objects are
usable only at their documented `known_at` time: for example, swings after
the confirming candle, FVGs at C3 close, and order blocks when the qualifying
FVG/BOS pair completes.

For a reference on another timeframe, use the latest candle whose **close
time is less than or equal to** the close time of `t`. Candle CSV timestamps
are UTC open times. Convert them to close times using the selected Binance
interval; do not compare open times as if they were close times. At a shared
close timestamp, update all engines for all newly closed candles first, then
evaluate signals in stable timeframe and strategy-ID order. This avoids
results that depend on thread scheduling.

Strategy steps follow their ordered `signals_required` sequence and its
`max_candles_after_previous_step` rules. An optional step may be skipped.
Freeze a step's custom level when that step becomes true. Freeze the resolved
entry, stop, and target references at the signal close; do not re-resolve them
from later engine state. The ATR used for structured buffers or a fallback
stop is the entry-timeframe ATR(14) known at signal time.

Use the contract's New York time conversion and candle-open semantics for
`ny_minute`, `ny_dow`, sessions, and kill zones. Engine bias is structure
bias only, never a substitute for presenter bias.

## 4. Entry, fill, stop, target, exit and R formulas

### Entry and order lifecycle

An entry signal is an occurrence on the close of `t`.

- A structured `limit` entry places a pending order at the resolved reference
  at the close of `t`. It can fill only on a later entry-timeframe candle,
  at the order price, when that candle's range touches the price. It expires
  after `BT_ORDER_EXPIRY_CANDLES` later candles.
- If the entry rule is null or has no structured rule, enter at the open of
  `t+1`. The next candle must exist; otherwise there is no fill.
- A signal whose referenced level cannot be resolved is
  `unresolvable_ref`, not a default-price trade.
- A pending order is cancelled as `invalidated_before_fill` if a computable
  strategy invalidation becomes true. It is also cancelled if its planned
  stop is reached before the entry. Since OHLC does not reveal the order of
  prices within a candle, simultaneous entry/stop touches are conservatively
  treated as stop-before-entry and cancelled.
- A still-pending order is `expired_unfilled` after its final eligible candle.

The catalog's structured types `stop`, `beyond`, and `level` do not specify
all execution semantics. The provisional v1 mapping is: `stop` and `beyond`
are stop-market triggers at their resolved reference (including
`buffer_atr`); `level` is a touch/limit order at its reference. For a trigger
gapped through at the open, the provisional fill is the trigger price.
These mappings are **UNVERIFIED** and listed as open questions in section 12.

### Stop and target

For a long, stop/target distances extend below/above entry; for a short, the
directions reverse. A structured stop uses `stop_rule.structured.reference`
and its `buffer_atr` times the entry-timeframe ATR(14). A missing or
unstructured stop uses
`BT_DEFAULT_STOP_ATR * ATR(14)` beyond entry. A structured target uses its
resolved target level, or a numeric `rr_multiple` times risk distance. A
missing or unstructured target uses `BT_DEFAULT_TARGET_R`.

Set `stop_exit_model` and `target_exit_model` independently to `native` when
that component has a structured rule, otherwise `default`. The trade-level
`exit_model` is `native` when both are native, `default` when both use
defaults, and `mixed` otherwise. Do not substitute defaults when a declared
native reference is unresolvable.

`risk_distance = abs(entry_price - stop_price)` and must be positive;
otherwise record `unresolvable_ref`. `risk_bps =
10000 * risk_distance / entry_price`. Planned reward-to-risk is
`abs(target_price - entry_price) / risk_distance`.

### Fill-candle and later-candle processing

On the fill candle, check **only the stop**. A stop touch is a loss at `-1R`;
ignore any target touch on this candle. For every later candle, test the
range against stop and target. If both are reachable in the same candle,
apply decision 1 (`tie_loss`). A stop-only touch exits at the stop price;
a target-only touch exits at the target price. No intrabar order is inferred.

Count the fill candle as trade bar 1. At the close of bar
`BT_MAX_BARS_IN_TRADE`, if neither stop nor target has resolved, exit at that
candle's close with `exit_reason=timeout`. A still-open position at the final
data close exits at the last close with `exit_reason=end_of_data` and
`status=open_at_end`.

Strategy management text, partial exits, and dynamic breakeven/trailing
changes are not applied in v1. The ledger describes one fixed-size,
single-entry, single-exit outcome.

### R and excursion formulas

Let `s=+1` for long and `s=-1` for short, `d=risk_distance`, and `C` be the
chosen round-trip cost in basis points:

- `r_gross = s * (exit_price - entry_price) / d`
- `cost_r = C * entry_price / 10000 / d`
- `r_net = r_gross - cost_r`
- `r_std2_net = r_std2_gross - cost_r`, where `r_std2_gross` uses the same
  stop and a target exactly `2R` from entry, with the same fill-candle,
  tie, timeout, and end-of-data rules.
- Long `mfe_r = (max_high_after_fill - entry_price) / d`;
  short `mfe_r = (entry_price - min_low_after_fill) / d`.
- Long `mae_r = (min_low_after_fill - entry_price) / d`;
  short `mae_r = (entry_price - max_high_after_fill) / d`.

Include the fill candle and all later candles through resolution in MFE/MAE.
They are range-based excursions, not claims about intrabar path. MAE is
normally non-positive; a gap can exceed the planned stop. The tie rule fixes
the gross tie result at `-1R`, irrespective of the candle's farther extreme.
Costs use the entry price and the same round-trip rate for native and
standardized outcomes.

### Worked examples

For illustration only, assume long entry `$100`, stop `$98`, risk distance
`$2`, round-trip cost `10 bps`, and therefore `cost_r=10*100/10000/2=0.05`.
The example prices and cost are arithmetic examples, not market or cost
defaults.

| Case | Path and exit | `r_gross` | `cost_r` | `r_net` | `r_std2_net` |
|---|---|---:|---:|---:|---:|
| Win | Target `$106` (3R); maximum high `$106.40`, minimum low `$99.60` | `+3.00` | `0.05` | `+2.95` | `+1.95` |
| Loss | Stop `$98` touched after fill; maximum high `$100.80` | `-1.00` | `0.05` | `-1.05` | `-1.05` |
| Tie loss | A later candle reaches both `$98` stop and `$106` target | `-1.00` | `0.05` | `-1.05` | `-1.05` |
| Timeout | After five trade bars, close `$102`; max high `$105`, min low `$99` | `+1.00` | `0.05` | `+0.95` | `+0.95` |
| Expired order | Limit `$100` is not touched during its three eligible candles | `null` | `null` | `null` | `null` |

For the timeout example, the standard 2R target was not reached, so it shares
the timeout exit. An unfilled occurrence has no risk or outcome values.

## 5. Applicability rules

Classify from category and rules:

- `entry_model`: an entry-category strategy with an entry rule; only these
  produce trade outcomes.
- `modifier`: category `market_condition_filter`; occurrence-only in v1,
  with optional modifier testing deferred.
- `framework`: category `context_framework`; occurrence-only in v1.
- `target_tool`: category `target_projection`; occurrence-only in v1.

Tier C is `excluded` from v1. Non-C occurrence-only roles remain eligible to
be logged, but have no win/loss outcome. A strategy may be N/A for a
particular run; N/A is never a loss.

Use the 15 `timeframes.labels` in `signal_catalog.json`. Expand `|` role
alternatives into separate combinations and retain only runs where
`bias_tf >= setup_tf >= entry_tf` in timeframe length. Resolve a null role
using the contract ladder: setup and entry inherit each other when only one
is specified; a null bias uses `ladder_up(setup_tf, 1)`. For a null-role
fractal strategy, create one run per supported execution timeframe, set
`setup_tf=entry_tf`, and derive bias from the ladder. A note that does not
define an execution timeframe and is not a fractal remains unresolved and
must be surfaced, not silently assigned one.

The manifest records each resolved run as `applicable`,
`not_applicable` (with reason), or `excluded` (with Tier C reason). Apply
custom-signal `valid_timeframes` transitively through each dependency
closure. For `pdh`/`pdl`, use `timeframes.pdh_pdl_validity`: treat misaligned
or degenerate timeframes as N/A; treat 2h/4h full-history use as N/A because
the contract permits it only during EDT. Session or time-window gating is
N/A when the entry timeframe is not among the intraday intervals for which
the contract supports that exact gate. Keep coarse kill-zone reads distinct
from exact session gating.

The manifest includes each strategy's full transitive custom-signal closure,
external dependencies, `needs_ny_time`, session/time-window requirements,
and K constants. Strategies excluded as Tier C stay visible in the manifest
and report with their reasons.

## 6. Ledger schema

CSV fields are UTF-8, stable column order, one row per occurrence (a
two-sided strategy emits separate directional occurrences). Nullable values
are empty CSV cells. `run_id` identifies the same immutable run in each
market-symbol file.

| Name | Type | Meaning |
|---|---|---|
| `occurrence_id` | string | Deterministic identity for one strategy/direction/signal-time occurrence. |
| `trade_id` | string/null | Deterministic occurrence-linked ID; null if no trade was taken. |
| `run_id` | string | Run identifier. |
| `market` | string | `binance` or `capital_com`. |
| `symbol` | string | Provider symbol from the run input. |
| `asset_class` | string | `crypto`, `forex`, `metals`, or `indices`; explicit input, not guessed from a ticker. |
| `cost_class` | string | Fine-grained cost bucket, e.g. `forex_majors`, `forex_crosses`, or `crypto_cfds`. |
| `strategy_id` | string | Unique ID from the source strategy JSON. |
| `role` | string | `entry_model`, `modifier`, `framework`, or `target_tool`. |
| `record_type` | string | `trade` for entry models; `occurrence` for occurrence-only roles. |
| `direction` | string | `long` or `short` after mirroring. |
| `entry_tf` | string | Resolved entry timeframe. |
| `setup_tf` | string | Resolved setup timeframe. |
| `bias_tf` | string | Resolved bias timeframe. |
| `split` | string | `train`, `validation`, `test`, or `unassigned`; assigned using the global calendar cutoffs. |
| `signal_open_time_ms` | int64 | UTC open time of the entry-timeframe signal candle. |
| `status` | enum | `taken`, `skipped_overlap`, `expired_unfilled`, `invalidated_before_fill`, `unresolvable_ref`, `open_at_end`, or `occurrence_only`. |
| `status_detail` | string/null | Machine-readable explanation for N/A execution, invalid reference, or cancellation. |
| `exit_model` | enum/null | `native`, `default`, or `mixed` across stop and target components. |
| `stop_exit_model` | enum/null | `native` or `default` stop component. |
| `target_exit_model` | enum/null | `native` or `default` target component. |
| `entry_type` | string/null | Structured order type or `market_next_open`. |
| `order_price` | float/null | Frozen pending order/trigger price. |
| `fill_time_ms` | int64/null | UTC close-time timestamp of the candle that first filled the order. |
| `fill_price` | float/null | Simulated entry fill price. |
| `stop_price` | float/null | Frozen stop price. |
| `target_price` | float/null | Frozen native or default target price. |
| `risk_bps` | float/null | `10000 * risk_distance / fill_price`. |
| `rr_planned` | float/null | Planned target distance divided by risk distance. |
| `exit_time_ms` | int64/null | UTC close-time timestamp of terminal or end-of-data exit. |
| `exit_price` | float/null | Simulated exit price. |
| `exit_reason` | enum/null | `stop`, `target`, `tie_loss`, `timeout`, or `end_of_data`. |
| `bars_in_trade` | int32/null | Entry-timeframe bars from fill through exit, including the fill candle. |
| `r_gross` | float/null | Native-model outcome before costs. |
| `round_trip_cost_bps` | float/null | Applied instrument cost rate, retained for auditability. |
| `cost_r` | float/null | Cost expressed in R. |
| `r_net` | float/null | Native-model outcome after costs. |
| `r_std2_net` | float/null | Same stop and cost, with a fixed 2R target. |
| `mfe_r` | float/null | Maximum favorable excursion through resolution, in R. |
| `mae_r` | float/null | Maximum adverse excursion through resolution, in R. |
| `kill_zone` | string/null | Engine kill-zone label at signal time; `none` when no zone applies. |
| `ny_dow` | int32/null | ISO weekday 1-7 at the signal candle's NY-local open time. |
| `ny_minute` | int32/null | NY-local minute of day at the signal candle's open time. |
| `engine_bias` | string | Engine bias on `entry_tf` at signal close. |
| `bias_tf_engine_bias` | string | Engine bias on `bias_tf` at the latest eligible close. |
| `atr_bps` | float/null | Entry-timeframe ATR(14) divided by signal close and multiplied by 10,000. |
| `source` | enum | `backtest` or `live`. |

Additions to the requested columns:

- `occurrence_id`, `trade_id`: keep repeated raw signals distinct and link an
  occurrence to its trade.
- `role`, `record_type`: represent frameworks, target tools, and modifiers
  without fabricating trade outcomes.
- `cost_class`: select the finer cost bucket without collapsing the coarser
  `asset_class` hierarchy used by Phase 4 statistics.
- `split`: preserve the locked chronological evaluation partition.
- `status_detail`: explain why an otherwise valid signal was not executable.
- `stop_exit_model`, `target_exit_model`: record `native` or `default` for
  each component while retaining the requested combined `exit_model`.
- `round_trip_cost_bps`: make `cost_r` independently auditable.
- `occurrence_only` status: distinguish a logged framework/modifier/target
  occurrence from an entry-model trade that was actually taken.

`fill_time_ms` and `exit_time_ms` are candle **close** times; `signal_open_time_ms`
is deliberately the signal candle's open time. For a pending/unfilled or
occurrence-only row, trade-specific fields are null. For `skipped_overlap`,
retain signal context and leave fill/outcome fields null.

`run_manifest.json` contains at least `run_id`, `git_commit`, spec version,
strategy/catalog versions, SHA-256 hashes of all
constants and costs inputs and every data file, engine constant names/values
with source references, market/symbol/timeframe inventory, global split
cutoffs, and CLI options including any test-unlock flag. Stable serialization,
ordered rows, sorted file traversal, and a run ID derived from input hashes
make reruns reproducible. Do not include wall-clock generation timestamps in
deterministic outputs.

## 7. Costs

Every entry in [`costs.json`](../strategies/catalog/costs.json) is a
user-review assumption and not a verified current fee schedule or spread.
Use the Binance fee type configured for the run (default taker) and add both
entry-side and exit-side slippage:

`round_trip_cost_bps = 2 * fee_bps_per_side + 2 * slippage_bps_per_side`.

For Capital.com, use the configured full bid/ask spread in basis points as
the round-trip price cost. Capital history is bid-only; the assumed spread
represents long entry at ask/exit at bid and short entry at bid/exit at ask.
Do not treat Capital tick-count volume as Binance base or quote volume and do
not pool the two markets without asset-class hierarchy.

For either market:

`cost_r = round_trip_cost_bps * entry_price / 10000 / risk_distance`.

**ASSUMPTION placeholders for review:** Binance USD-M maker fee 2 bps/side,
taker fee 5 bps/side, and slippage 1 bp/side; Capital.com round-trip spreads
of 2 bps for forex majors, 4 for crosses, 8 for metals, 5 for indices, and
20 for crypto CFDs. Every numeric value is individually marked
`assumption: true` in `costs.json`.

## 8. Data hygiene and splits

Follow the authoritative CSV contract: accept only 23-column rows, treat the
header as unreliable, and deduplicate by candle timestamp keeping the first
valid row. Parse timestamp as UTC candle-open time. The established layout is
column 0 timestamp, column 1 base volume, columns 5-18 indicators, and columns
19-22 open/high/low/close. Use the candle inputs to replay the C++ engines;
do not treat stored indicator text as a substitute for the engine replay.
Malformed timestamps, wrong-width rows, and rows with invalid required OHLC
or volume fields are excluded and counted in run diagnostics. Sort retained
candles chronologically before replay.

`csv_history.cpp` currently skips the first physical line unconditionally;
the backtest reader must not inherit that behavior. Validate every line by
width and timestamp, allowing an unreliable header to fail ordinary row
validation rather than assuming it is the first line.

The data contract says history backfill starts five years before the request
date or at listing, and files are not trimmed; actual per-file coverage is
**UNVERIFIED** and must be hashed and reported. Capital.com bid-only prices
and tick-count volume remain a separate market data source.

Use one global calendar start, cutoffs, and end for all symbols so correlated
symbols cannot receive different percentage-based boundaries. Proposed split
fractions are **ASSUMPTIONS**: train 70%, validation 15%, test 15% in
chronological order. For each entry timeframe, set an embargo at least as
long as the maximum holding duration (`BT_MAX_BARS_IN_TRADE` candles on that
timeframe); remove boundary-crossing trades and keep an embargo interval
between partitions. Calculate monthly durations from actual calendar-month
candles, not a guessed fixed number of days. If history is too short after
embargo, report the split/cell as insufficient rather than shortening the
gap.

The test split is locked. Statistics tooling must refuse test metrics unless
the caller supplies an explicit unlock flag (proposed CLI spelling:
`--unlock-test`). Unlocking never changes or overwrites the ledger.

Keep `asset_class` as a hierarchy level. Do not pool Binance and Capital.com
blindly: Capital prices are bid-only and Capital volume is a tick count.

## 9. Statistics (Phase 4)

Compute per `(strategy, symbol, entry_tf, split)` cell:

- `n_raw`: all signal occurrences, including `skipped_overlap`,
  `expired_unfilled`, `invalidated_before_fill`, and `unresolvable_ref`.
- `n_taken`: filled entry models; report resolved outcomes separately from
  `open_at_end`.
- Net win rate from closed trades (`r_net > 0`) with a Beta posterior.
  Proposed **ASSUMPTION** prior is `Beta(1,1)`; report the posterior mean and
  a 95% credible interval.
- Net expectancy in R with a 95% block-bootstrap interval.
- Profit factor from net R: sum of positive `r_net` divided by the absolute
  sum of negative `r_net`; report undefined when there are no losses.
- Maximum drawdown from the cumulative, chronological net-R curve.
- Signals per month from `n_raw` divided by the cell's eligible calendar
  months.

De-cluster overlapping holding intervals within
`(strategy, asset_class, entry_tf)`, including symbols in that asset class:
overlapping trade intervals form one cluster, whose return is the mean net R
of its members. Report cluster count as effective `n`; never treat correlated
simultaneous symbols as independent observations. Use that clustered sample
for intervals and ranking. Proposed **ASSUMPTION** minimums are 30 effective
observations for a cell, 100 for strategy-by-asset-class, and 200 for a
strategy-wide weight. Below threshold, show the estimate but mark it
insufficient and do not rank it.

Hierarchical shrinkage proceeds cell -> strategy-by-asset-class -> strategy.
For each level, shrink the estimate toward its parent by
`n_eff / (n_eff + k)`. Proposed **ASSUMPTIONS** are `k=30` at the cell level
and `k=100` at the strategy-by-asset-class level. Rank on the conservative
5th percentile/lower bound of the shrunk net expectancy, not its mean.

Compare every ranked cell with a random-entry baseline sampled on the same
symbol, timeframe, split, and eligible candle set. Match the strategy's
entry count, order type, stop/target model, costs, and overlap rules; use a
fixed seed in the run manifest. Do not let the baseline see future data.

For live online updates, decay the previous conjugate sufficient statistics
by a forgetting factor before adding each newly closed live trade. Proposed
**ASSUMPTION** is `lambda=0.995` per closed trade. Update a Beta win/loss
posterior with `r_net > 0` as the win event and a conjugate Normal-Inverse-Gamma
posterior with the observed `r_net` for expectancy and variance. Derive the
live weight from its conservative lower bound using the same hierarchy.
Keep live rows in a separate append-only ledger with `source=live`; compare
their rolling statistics with frozen historical weights to measure drift.
Do not silently rewrite historical ledger rows or historical statistics.

All statistical thresholds, priors, shrinkage values, split fractions, and
forgetting factors above are proposals, not verified market facts.

## 10. Live-path compatibility notes

The live evaluator must call the same C++ strategy evaluator and simulator
semantics as Stage 1. It must maintain per-timeframe `IndicatorEngine` and
`SmcEngine` state and coordinate closed candles before evaluating
cross-timeframe references. It reads the same strategy/catalog/constants and
cost configuration versions recorded in the historical `run_manifest.json`.

Historical weights are read-only inputs for live scoring. A closed live trade
is appended with `source=live` to a separate ledger using the identical
schema; the online update changes live weights, not the historical baseline.
Record engine biases as separate context columns. Presenter bias remains
non-computable and must not become an implicit live gate.

## 11. Phase plan for implementation

0. **Specification and manifest (this phase):** freeze this contract, write
   the deterministic manifest builder, review assumed costs/constants, and
   resolve open questions before engine work.
1. **C++ replay:** inventory data inputs, implement strict CSV hygiene,
   chronological multi-timeframe engine replay, run metadata/hashes, and
   deterministic event ordering.
2. **C++ evaluator:** implement mirror-based direction evaluation, ordered
   signal steps, custom signal dependencies, applicability gates, and
   occurrence logging.
3. **C++ simulator and ledger:** implement order lifecycle, stop/target/tie
   rules, fixed 2R companion result, costs, overlap control, and row schema.
4. **Python statistics:** validate ledger schema and hashes, honor locked
   test splits, calculate cell/hierarchical statistics, random baseline,
   and versioned weights.
5. **Live reuse:** connect the same evaluator/ledger semantics to the live
   candle stream, isolate live rows, implement forgetting updates, and add
   drift monitoring.

## 12. Open questions

1. **Order-type semantics (UNVERIFIED):** `stop`, `beyond`, and `level` are
   allowed structured types, but the data contract does not define fill
   behavior for each. Review the provisional model and affected entry rules,
   especially `smt_divergence_confirmation` and
   `trending_turtle_soup_strength` (`stop`), and any strategy using `beyond`
   or `level`. Also confirm stop-before-entry handling when one candle spans
   both levels.
2. **Tier and external tags:** the lint report has six Tier C rows, including
   non-computable projections/targets as well as external-symbol cases.
   Confirm that all lint Tier C entries remain excluded even when the reason
   is a null custom signal, especially `measured_swing_projections`,
   `standard_deviations_indices`, `sweep_or_run`, and
   `hp_lp_conditions_filter`. The registry marks some clock-based signals
   `market:needs_equity_session`; confirm whether these require external
   equity-session data or are computable from NY timestamps, notably
   `arjoio_ny_am_session`, `equities_open_judas`, and
   `indices_session_sequence`.
3. **PDH/PDL edge cases:** contract validity is EDT-only on 2h/4h and
   degenerate on daily and higher. This spec excludes those full-history
   runs; confirm whether seasonal 2h/4h partitions should instead be split
   by EDT/EST. The current strategy dependency scan found no direct `pdh` or
   `pdl` references; review the validity policy in `signal_catalog.json` and
   the intraday previous-day logic in `weekly_profiles_tgif`.
4. **Execution costs:** no verified fee tier, maker/taker mix, slippage, or
   Capital.com spread schedule was found in the repository. Review every
   placeholder in `costs.json`, especially the asset-class mapping and
   whether limit entries qualify for maker fees.
5. **Dataset inventory:** the exact 22 symbols, provider symbol names,
   `market` values, `asset_class` mapping, and CSV paths are UNVERIFIED.
   Supply the inventory before implementation; do not infer asset class
   solely from symbol text.
6. **Split feasibility:** the repo contract states a five-year requested
   history window, while a 20-candle monthly trade can require an embargo of
   roughly 20 calendar months. Confirm whether all 15 timeframes need
   independent splits and accept insufficient monthly validation/test cells.
7. **Management and partials:** fixed single-exit outcomes omit strategy
   management notes and partial/breakeven/trailing behavior. Confirm v1's
   exclusion for strategies with such notes, including `m3_secrets`,
   `swing_trading_lost_art`, `new_york_reversal`, and
   `swing_rate_silver_bullet`.
8. **Live cross-timeframe state:** the current contract says streams have
   independent timeframe state and no cross-timeframe state. Confirm how the
   live process will supply synchronized closed candles to the shared
   evaluator without changing historical semantics.
9. **Statistical assumptions:** review the 70/15/15 split, per-timeframe
   embargo, priors, minimum effective-n thresholds, shrinkage values,
   baseline sampling, and `lambda=0.995`; these are proposals, not repo facts.
10. **Unresolved filter timeframes:** `hp_lp_conditions_filter` names only
    higher-timeframe bias alternatives and leaves setup/entry null without a
    resolution note. Define whether to log only its bias-context occurrence
    or which lower role should be paired with each bias timeframe.
