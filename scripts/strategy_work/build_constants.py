import json, os

E = "engine"
CONSTANTS = [
    ("K_DISPLACEMENT_BODY_MULT", 1.5, "multiple of the rolling average candle body",
     "Engine displacement test: body >= 1.5 x rolling average body (FVG C2 and CHoCH->MSS). Reused for every presenter 'displacement/expansion/strong move' proxy.",
     [1.2, 2.5], "src/smc-ict/fair_value_gaps.h:FvgTracker::kDisplacementMultiplier"),
    ("K_DISPLACEMENT_LOOKBACK", 20, "candles (same timeframe)",
     "Window of the rolling average body. For the FVG displacement test the engine averages the 20 bodies before the judged candle, reproduced as avg(body[-1], 20); the CHoCH->MSS test lags one more candle.",
     [10, 50], "src/smc-ict/smc_engine.h:SmcEngine::rollingBodyAvg_{20}"),
    ("K_EQUAL_LEVEL_ATR", 0.1, "ATR(14) multiple of the evaluated timeframe",
     "Engine tolerance for 'the same price' (equal highs/lows). Reused for 'a level coincides with a PD array'.",
     [0.05, 0.5], "src/smc-ict/liquidity.h:LiquidityTracker::kEqualLevelAtrMultiplier"),
    ("K_BPR_RECENCY_CANDLES", 10, "candles", "Engine window for opposite FVGs forming a BPR.",
     [3, 30], "src/smc-ict/fair_value_gaps.h:FvgTracker::kBprRecencyWindowCandles"),
    ("K_REJECTION_WICK_FRACTION", 0.33, "wick / range ratio", "Engine minimum wick share for a Rejection Block ('long wick').",
     [0.25, 0.6], "src/smc-ict/order_blocks.h:OrderBlockTracker::kMinRejectionWickFraction"),
    ("K_OB_LOOKBACK_CANDLES", 64, "candles", "Engine lookback for an OB anchor / qualifying FVG and BOS.",
     [20, 128], "src/smc-ict/order_blocks.h:OrderBlockTracker::kLookbackCandles"),
    ("K_ST_MAX_CANDLES", 20, "candles (setup timeframe)",
     "Presenters give no bound between the first and second FVG of the ST model ('displacement, retracement, displacement'); 20 candles keeps both FVGs inside one intraday leg on 1m-15m.",
     [5, 60], None),
    ("K_TAP_LOOKBACK_CANDLES", 12, "candles (step timeframe)",
     "How long after an HTF array tap the 'first FVG away from the array' still counts; presenters say 'the very first FVG' without a time bound.",
     [3, 48], None),
    ("K_CONSOLIDATION_CANDLES", 12, "candles", "Window for the consolidation proxy (N-candle range <= K x ATR).", [6, 48], None),
    ("K_CONSOLIDATION_RANGE_ATR", 2.0, "ATR(14) multiple",
     "Consolidation proxy: a 12-candle high-low range no larger than 2 ATR means no candle expanded the range materially.",
     [1.0, 4.0], None),
    ("K_REACTION_WINDOW_CANDLES", 3, "candles", "Proxy for 'immediately' after a sweep (reaction/rebalance must start within N candles).",
     [1, 10], None),
    ("K_STOP_BUFFER_ATR", 1.0, "ATR(14) multiple of the entry timeframe",
     "Machine form of market-specific stop room the presenter gives in pips/points ('room to breathe'); default 1 ATR is a placeholder to be swept.",
     [0.25, 2.0], None),
    ("K_OPEN_WINDOW_MINUTES", 30, "minutes (NY local)",
     "Width of the window after the 08:30 / 09:30 NY equity-related opens in which a 'Judas' sweep is attributed to that open; the presenter names only the start times.",
     [15, 60], None),
]


def write():
    out = {"constants_version": "1.0.0", "constants": []}
    for name, default, unit, why, rng, reuse in CONSTANTS:
        out["constants"].append({"name": name, "default": default, "unit": unit, "rationale": why,
                                 "suggested_sweep_range": rng, "reuses_engine_constant": reuse,
                                 "provenance": "engine" if reuse else "inferred", "used_by": []})
    return out


if __name__ == "__main__":
    os.makedirs("strategies/catalog", exist_ok=True)
    json.dump(write(), open("strategies/catalog/constants.json", "w", encoding="utf-8"), indent=2, ensure_ascii=False)

