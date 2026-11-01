# Strategy backtest manifest report

- Strategy IDs: **39** across **35** strategy JSON files.
- The expected **35** refers to source files, not IDs; the manifest's 39 IDs match the 39 rows in the lint report. If 35 was intended as an ID count, the repo contains four additional IDs.
- Four files contain two IDs each, accounting for the difference between 35 files and 39 IDs: dol_fibonacci.json (2), newyork_killzone.json (2), snd_turtlesoup.json (2), st_model.json (2).
- Roles: entry models **30**, modifiers **2**, frameworks **5**, target tools **2**.
- Tiers: A **1**, B **32**, C **6**; v1 included **33**, excluded **6**.
- Resolved timeframe runs: **504**; excluded tier-C runs **167**; not-applicable runs **28**.
- Unresolved timeframe roles: hp_lp_conditions_filter.
- Structured rules across all IDs: stops **21**, targets **19**. Entry-model native/default counts: stops **20/10**, targets **15/15**.

| Strategy | File | Category | Role | Tier | v1 status | Entry rule | Native stop | Native target | Timeframe runs |
|---|---|---|---|---:|---|---|---:|---:|---:|
| advanced_market_structure | advanced_ms.json | trend_following | entry_model | B | included | yes | yes | yes | 15 |
| amd_power_of_3 | amd_po3.json | session_open_manipulation | entry_model | A | included | yes | no | no | 15 |
| bias_independent | bias_independance.json | htf_continuation | entry_model | B | included | yes | yes | yes | 2 |
| breaker_block_setup | breaker_block.json | liquidity_reversal | entry_model | B | included | yes | no | no | 24 |
| daily_bias_via_fvg_reading | daily_bias.json | htf_continuation | entry_model | B | included | yes | no | yes | 1 |
| daily_profiles | daily_profile.json | context_framework | framework | B | included | yes | no | yes | 3 |
| dealing_range_reversal | dealing_ranges.json | liquidity_reversal | entry_model | B | included | yes | yes | no | 1 |
| external_internal | external_internal.json | liquidity_reversal | entry_model | B | included | yes | yes | yes | 15 |
| flod_llod | flod_llod.json | context_framework | framework | B | included | no | yes | no | 15 |
| gap_and_imbalance_overlap_confluence | gap_imbalance.json | context_framework | framework | B | included | yes | no | no | 15 |
| hp_lp_conditions_filter | trading_probabilities.json | market_condition_filter | modifier | C | excluded | yes | no | yes | 0 |
| htf_boundary_scalping | advanced_scalping.json | htf_continuation | entry_model | B | included | yes | yes | yes | 4 |
| indices_session_sequence | trading_indices.json | session_open_manipulation | entry_model | B | included | yes | yes | no | 4 |
| judas_swing_reversal | avoid_fakouts.json | session_open_manipulation | entry_model | B | included | yes | yes | no | 4 |
| liquidity_sweep_and_target | liquidity_pools.json | liquidity_reversal | entry_model | B | included | yes | no | no | 15 |
| london_killzone | london_killzone.json | htf_continuation | entry_model | B | included | yes | yes | yes | 1 |
| m3_secrets | m3_secrets.json | htf_continuation | entry_model | B | included | yes | yes | yes | 38 |
| market_structure_secrets | ms_secrets.json | trend_following | entry_model | B | included | yes | yes | yes | 15 |
| measured_swing_projections | dol_fibonacci.json | target_projection | target_tool | C | excluded | yes | no | yes | 45 |
| mitigation_block_setup | mitigation_block.json | trend_following | entry_model | B | included | yes | no | no | 15 |
| new_york_retracement | newyork_killzone.json | htf_continuation | entry_model | B | included | yes | yes | yes | 8 |
| new_york_reversal | newyork_killzone.json | session_open_manipulation | entry_model | B | included | yes | yes | yes | 8 |
| order_block_mitigation | order_block.json | trend_following | entry_model | B | included | yes | no | no | 15 |
| overlapping_pd_array_entry | entry_patterns.json | htf_continuation | entry_model | B | included | yes | yes | yes | 6 |
| reclaimed_order_block_overlap | reclaimed_ob.json | trend_following | entry_model | B | included | yes | no | no | 1 |
| rejection_block_reversal | rejection_block.json | liquidity_reversal | entry_model | B | included | yes | no | no | 15 |
| relative_strength_filtering | intermarket_strength.json | trend_following | entry_model | C | excluded | yes | no | no | 26 |
| seek_and_destroy_turtle_soup | snd_turtlesoup.json | mean_reversion | entry_model | B | included | yes | yes | no | 1 |
| seek_n_destroy | seek_destroy.json | market_condition_filter | modifier | B | included | yes | no | no | 3 |
| smt_divergence_confirmation | smt_divergence.json | indicator_confluence | entry_model | C | excluded | yes | yes | no | 15 |
| st_model_advanced | st_model.json | liquidity_reversal | entry_model | B | included | yes | yes | no | 15 |
| st_model_standard | st_model.json | liquidity_reversal | entry_model | B | included | yes | yes | yes | 15 |
| standard_deviations_indices | dol_fibonacci.json | target_projection | target_tool | C | excluded | yes | no | yes | 66 |
| sweep_or_run | sweep__run.json | context_framework | framework | C | excluded | yes | no | no | 15 |
| swing_rate_silver_bullet | silver_bullet.json | htf_continuation | entry_model | B | included | yes | yes | yes | 15 |
| swing_trading_lost_art | shorterm_trading.json | htf_continuation | entry_model | B | included | yes | yes | yes | 9 |
| swings_hl | swing_hl.json | liquidity_reversal | entry_model | B | included | yes | no | no | 21 |
| trending_turtle_soup_strength | snd_turtlesoup.json | trend_following | entry_model | B | included | yes | yes | yes | 1 |
| weekly_profiles_tgif | weekly_profile.json | context_framework | framework | B | included | yes | no | no | 2 |

A structured rule is counted as native even when its reference is a custom signal. A null or unstructured rule uses the specified backtest default only for entry models; framework, modifier, and target-tool rows are occurrence-only.
