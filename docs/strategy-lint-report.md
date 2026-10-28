# strategy_lint report

Errors: 0  Warnings: 64

Tier counts: A=1 B=32 C=6

| strategy | file | tier | timeframes | engine signals | custom signals | non-computable steps |
|---|---|---|---|---|---|---|
| advanced_market_structure | advanced_ms.json | B | - | candle.high, structural_event.kind | htf_opposing_pd_array, intermediate_term_high, intermediate_term_low | 0 |
| htf_boundary_scalping | advanced_scalping.json | B | 15m, 1d, 1h, 1m, 5m | fvg.bullish | intermediate_term_low, setup_tf_pd_array_tap, short_term_swing_point | 0 |
| amd_power_of_3 | amd_po3.json | A | - | structural_event.kind | - | 0 |
| judas_swing_reversal | avoid_fakouts.json | B | 15m, 1d, 1m, 4h, 5m | - | judas_reversal_zone_tap, llod | 0 |
| bias_independent | bias_independance.json | B | 1h, 1m, 5m | - | fvg_touched, htf_pd_array_tap, intermediate_term_high, short_term_low | 0 |
| breaker_block_setup | breaker_block.json | B | 1d, 1h, 1mo, 1w, 4h, 5m | - | breaker_pattern, unicorn_overlap | 0 |
| daily_bias_via_fvg_reading | daily_bias.json | B | 1d, 1w | fvg.bullish | fvg_respected, fvg_touched, htf_opposing_pd_array, htf_pd_array_tap | 0 |
| daily_profiles | daily_profile.json | B | 15m, 1d, 1h, 4h | - | displacement_candle, htf_opposing_pd_array, htf_pd_array_tap | 0 |
| dealing_range_reversal | dealing_ranges.json | B | 15m, 1d | - | dealing_range, dealing_range_low, fvg_without_displacement, htf_pd_array_tap, quadrant_extreme | 0 |
| measured_swing_projections | dol_fibonacci.json | C | 15m, 1d, 1h, 1m, 1w, 30m, 4h, 5m | - | judas_swing, measured_swing_projection, overlapping_pd_array | 0 |
| standard_deviations_indices | dol_fibonacci.json | C | 15m, 1d, 1h, 1m, 1w, 30m, 4h, 5m | - | judas_swing, overlapping_pd_array, standard_deviations | 0 |
| overlapping_pd_array_entry | entry_patterns.json | B | 1d, 1h, 1m, 5m | - | htf_pd_array_tap, intermediate_term_high, intermediate_term_low, overlapping_pd_array | 0 |
| external_internal | external_internal.json | B | - | candle.high, fvg.bullish, structural_event.kind | fvg_touched, htf_opposing_pd_array, htf_pd_array_tap, short_term_high | 0 |
| flod_llod | flod_llod.json | B | - | fvg.bullish, order_block.bullish | llod, overlapping_pd_array | 0 |
| gap_and_imbalance_overlap_confluence | gap_imbalance.json | B | - | - | fvg_without_displacement, overlapping_pd_array | 0 |
| relative_strength_filtering | intermarket_strength.json | C | 1d, 1w | - | decoupled_market, relatively_stronger_pair | 0 |
| liquidity_sweep_and_target | liquidity_pools.json | B | - | structural_event.kind | displacement_reaction, pure_sweep | 0 |
| london_killzone | london_killzone.json | B | 15m, 1h, 4h, 5m | fvg.bullish | fvg_touched, htf_pd_array_tap, intermediate_term_low_stop, london_kz_entry_trigger | 0 |
| m3_secrets | m3_secrets.json | B | 15m, 1d, 1h, 1mo, 1w, 4h, 5m | fvg.bullish | first_fvg_after_array_tap, fvg_touched, htf_opposing_pd_array, htf_pd_array_tap, short_term_range | 0 |
| mitigation_block_setup | mitigation_block.json | B | - | candle.low | mb_fvg_overlap_sensitivity, mitigation_block_pd_array_overlap, swing_failure_pattern | 0 |
| market_structure_secrets | ms_secrets.json | B | - | fvg.bullish | first_fvg_after_array_tap, fvg_touched, htf_pd_array_tap, intermediate_term_high, short_term_range | 0 |
| new_york_reversal | newyork_killzone.json | B | 15m, 1d, 1h, 1m, 1mo, 4h, 5m | fvg.bullish | intermediate_term_low, london_missed_htf_poi, ny_taps_htf_poi, overlapping_pd_array | 0 |
| new_york_retracement | newyork_killzone.json | B | 15m, 1d, 1h, 1m, 1mo, 4h, 5m | fvg.bullish | intermediate_term_low, london_taps_htf_poi_and_displaces, overlapping_pd_array | 0 |
| order_block_mitigation | order_block.json | B | - | candle.low | ob_fvg_overlap_sensitivity, presenter_order_block | 0 |
| reclaimed_order_block_overlap | reclaimed_ob.json | B | 15m, 4h | order_block.bullish | market_maker_curve_shift, reclaimed_pd_array_overlap, reclaimed_trade_through | 0 |
| rejection_block_reversal | rejection_block.json | B | - | - | rejection_block, rejection_block_body_sweep | 0 |
| seek_n_destroy | seek_destroy.json | B | 1d, 4h | structural_event.kind | equilibrium_reversion, pd_array_failure | 0 |
| swing_trading_lost_art | shorterm_trading.json | B | 1d, 1h, 1mo, 1w, 4h | fvg.bullish | htf_pd_array_tap, intermediate_term_low, overlapping_pd_array, short_term_range | 0 |
| swing_rate_silver_bullet | silver_bullet.json | B | - | candle.high, fvg.bullish | intermediate_term_high, swing_rate_050_behavior, swing_rate_075 | 0 |
| smt_divergence_confirmation | smt_divergence.json | C | - | structural_event.kind | htf_pd_array_tap, smt_divergence | 1 |
| seek_and_destroy_turtle_soup | snd_turtlesoup.json | B | 1d, 5m | structural_event.kind | intermediate_term_low, pure_sweep, quadrant_extreme, st_model | 0 |
| trending_turtle_soup_strength | snd_turtlesoup.json | B | 1h, 4h, 5m | structural_event.kind | htf_pd_array_tap, pure_sweep | 0 |
| st_model_standard | st_model.json | B | - | fvg.bullish | first_fvg_after_array_tap, fvg_touched, htf_pd_array_tap, intermediate_term_high, intermediate_term_low | 0 |
| st_model_advanced | st_model.json | B | - | fvg.bullish | first_fvg_after_array_tap, fvg_touched, htf_pd_array_tap, intermediate_term_low | 0 |
| sweep_or_run | sweep__run.json | C | - | - | displacement_candle, fvg_respected, liquidity_run_continuation, obvious_draw_on_liquidity, pure_sweep, st_model | 0 |
| swings_hl | swing_hl.json | B | 15m, 1d, 1h, 4h, 5m | structural_event.kind | short_term_high | 0 |
| indices_session_sequence | trading_indices.json | B | 15m, 1d, 1m, 4h, 5m | fvg.bullish, structural_event.kind | pure_sweep | 0 |
| hp_lp_conditions_filter | trading_probabilities.json | C | 1d, 1mo, 1w, 4h | - | consolidation_expansion_consolidation, displacement_candle, failed_rejection, major_expansion_dol_swept, obvious_draw_on_liquidity, pre_major_news | 0 |
| weekly_profiles_tgif | weekly_profile.json | B | 15m, 1d, 1h, 4h | - | friday_tgif_reversal, htf_pd_array_tap | 0 |

## Errors


## Warnings

- advanced_scalping.json:htf_boundary_scalping:filters[0]: no numeric_definition
- amd_po3.json:amd_power_of_3:custom_signals.monthly_open: numeric_definition is null (not computable)
- amd_po3.json:amd_power_of_3:invalidation[0]: no numeric_definition
- amd_po3.json:amd_power_of_3:filters[0]: no numeric_definition
- amd_po3.json:amd_power_of_3:filters[1]: no numeric_definition
- avoid_fakouts.json:judas_swing_reversal:filters[0]: no numeric_definition
- avoid_fakouts.json:judas_swing_reversal:filters[1]: no numeric_definition
- bias_independance.json:bias_independent:filters[1]: no numeric_definition
- breaker_block.json:breaker_block_setup:invalidation[0]: no numeric_definition
- breaker_block.json:breaker_block_setup:filters[0]: no numeric_definition
- breaker_block.json:breaker_block_setup:filters[1]: no numeric_definition
- daily_bias.json:daily_bias_via_fvg_reading:invalidation[1]: no numeric_definition
- daily_bias.json:daily_bias_via_fvg_reading:filters[1]: no numeric_definition
- dealing_ranges.json:dealing_range_reversal:invalidation[1]: no numeric_definition
- dealing_ranges.json:dealing_range_reversal:filters[1]: no numeric_definition
- dol_fibonacci.json:measured_swing_projections:custom_signals.measured_swing_projection: numeric_definition is null (not computable)
- dol_fibonacci.json:measured_swing_projections:custom_signals.monthly_open: numeric_definition is null (not computable)
- dol_fibonacci.json:measured_swing_projections:invalidation[0]: no numeric_definition
- dol_fibonacci.json:measured_swing_projections:filters[1]: no numeric_definition
- dol_fibonacci.json:standard_deviations_indices:custom_signals.standard_deviations: numeric_definition is null (not computable)
- dol_fibonacci.json:standard_deviations_indices:filters[0]: no numeric_definition
- external_internal.json:external_internal:invalidation[0]: no numeric_definition
- external_internal.json:external_internal:filters[1]: no numeric_definition
- gap_imbalance.json:gap_and_imbalance_overlap_confluence:invalidation[1]: no numeric_definition
- gap_imbalance.json:gap_and_imbalance_overlap_confluence:filters[1]: no numeric_definition
- intermarket_strength.json:relative_strength_filtering:custom_signals.decoupled_market: numeric_definition is null (not computable)
- intermarket_strength.json:relative_strength_filtering:custom_signals.relatively_stronger_pair: numeric_definition is null (not computable)
- intermarket_strength.json:relative_strength_filtering:filters[0]: no numeric_definition
- intermarket_strength.json:relative_strength_filtering:filters[1]: no numeric_definition
- liquidity_pools.json:liquidity_sweep_and_target:filters[1]: no numeric_definition
- london_killzone.json:london_killzone:filters[0]: no numeric_definition
- london_killzone.json:london_killzone:filters[1]: no numeric_definition
- london_killzone.json:london_killzone:filters[2]: no numeric_definition
- m3_secrets.json:m3_secrets:invalidation[1]: no numeric_definition
- m3_secrets.json:m3_secrets:filters[1]: no numeric_definition
- m3_secrets.json:m3_secrets:filters[2]: no numeric_definition
- mitigation_block.json:mitigation_block_setup:invalidation[0]: no numeric_definition
- newyork_killzone.json:new_york_reversal:filters[1]: no numeric_definition
- newyork_killzone.json:new_york_retracement:filters[0]: no numeric_definition
- order_block.json:order_block_mitigation:filters[0]: no numeric_definition
- order_block.json:order_block_mitigation:filters[1]: no numeric_definition
- shorterm_trading.json:swing_trading_lost_art:invalidation[0]: no numeric_definition
- shorterm_trading.json:swing_trading_lost_art:filters[0]: no numeric_definition
- shorterm_trading.json:swing_trading_lost_art:filters[2]: no numeric_definition
- silver_bullet.json:swing_rate_silver_bullet:invalidation[0]: no numeric_definition
- silver_bullet.json:swing_rate_silver_bullet:filters[0]: no numeric_definition
- smt_divergence.json:smt_divergence_confirmation:filters[2]: no numeric_definition
- snd_turtlesoup.json:seek_and_destroy_turtle_soup:invalidation[0]: no numeric_definition
- snd_turtlesoup.json:seek_and_destroy_turtle_soup:filters[0]: no numeric_definition
- snd_turtlesoup.json:seek_and_destroy_turtle_soup:filters[1]: no numeric_definition
- snd_turtlesoup.json:trending_turtle_soup_strength:invalidation[0]: no numeric_definition
- snd_turtlesoup.json:trending_turtle_soup_strength:filters[0]: no numeric_definition
- snd_turtlesoup.json:trending_turtle_soup_strength:filters[1]: no numeric_definition
- sweep__run.json:sweep_or_run:custom_signals.obvious_draw_on_liquidity: numeric_definition is null (not computable)
- sweep__run.json:sweep_or_run:filters[0]: no numeric_definition
- trading_indices.json:indices_session_sequence:filters[1]: no numeric_definition
- trading_probabilities.json:hp_lp_conditions_filter:custom_signals.obvious_draw_on_liquidity: numeric_definition is null (not computable)
- trading_probabilities.json:hp_lp_conditions_filter:invalidation[0]: no numeric_definition
- weekly_profile.json:weekly_profiles_tgif:invalidation[0]: no numeric_definition
- weekly_profile.json:weekly_profiles_tgif:filters[0]: no numeric_definition
- weekly_profile.json:weekly_profiles_tgif:filters[1]: no numeric_definition
- constants.json: unused constant K_BPR_RECENCY_CANDLES
- constants.json: unused constant K_OB_LOOKBACK_CANDLES
- constants.json: unused constant K_REJECTION_WICK_FRACTION
