from semutil import *


def apply(doc, log, ctx):
    s = strategy(doc, "flod_llod")
    set_category(s, "context_framework", log)
    set_field(
        s,
        "summary",
        "Map the current price leg from its origin swing low to its confirmed swing high, then mark the first retracement PD array as the first line of defense. The higher-probability case is a bullish FVG overlapping a bullish order block, while the origin swing low is the last line of defense.",
        log,
    )
    s["timeframes"]["notes"] = "Fractal concept. setup_tf is the leg timeframe being analysed; bearish examples are mirrored through the catalog mirror table."
    log.add("timeframes", "flod_llod: notes rewritten to treat setup_tf as the analysed leg timeframe and to state the bullish-side mirror convention")
    s["glossary"] = {
        "First line of defense": "FLOD (older spelling FLOT): the first PD array met on the retracement; in this lesson the higher-probability case is an FVG overlapping an order block, via custom_signals.flod and custom_signals.overlapping_pd_array.",
        "Last line of defense": "LLOD (older spelling LOT): the swing extreme at the origin of the current leg; see custom_signals.llod.",
        "Price action leg": "The swing low-to-swing high range being retraced; the transcript also says 'price action lag'.",
    }
    log.add("glossary", "flod_llod: canonicalized FLOD/LLOD terms, folded FLOT/LOT into aliases, and corrected 'price action lag' -> 'price action leg'")
    s["signals_required"] = [
        step(1, "custom_signals.llod", "equals", True, "setup_tf", "01:07"),
        step(2, "fvg.bullish", "equals", True, "setup_tf", "01:41"),
        step(3, "order_block.bullish", "equals", True, "setup_tf", "01:41", maxc=0),
        step(4, "custom_signals.overlapping_pd_array", "equals", True, "setup_tf", "02:08", maxc=0),
    ]
    log.add("signals_required", "flod_llod: raw FVG + OB overlap kept as steps, with llod added to define the active bullish leg and overlapping_pd_array naming the FLOD zone")
    s["custom_signals"] = [
        ctx.cs("flod", "02:08"),
        ctx.cs("llod", "02:25"),
        ctx.cs("overlapping_pd_array", "01:41"),
    ]
    log.add("custom_signals", "flod_llod: custom signals 'FLOD'/'LLOD' -> registry names flod/llod; added overlapping_pd_array for the overlap condition the presenter calls highest probability")
    s["sequence"] = [
        "Mark the current bullish price leg from its origin swing low to the confirmed swing high that created the retracement.",
        "Scan the retracement path for a bullish fair value gap and a bullish order block that share price.",
        "Treat that overlap as the first line of defense on the retracement.",
        "If price closes below the origin swing low, the leg has failed and a new opposite leg must be mapped.",
    ]
    set_numeric(s["invalidation"], ["close < custom_signals.llod"])
    set_numeric(s["filters"], ["custom_signals.flod AND custom_signals.overlapping_pd_array"])
    s["entry_rule"] = null_rule(s["entry_rule"])
    s["target_rule"] = null_rule(s["target_rule"])
    s["stop_rule"] = keep_rule(
        s["stop_rule"],
        text="For the bullish reading, price should not stay below the leg's origin swing low (the last line of defense).",
        structured={"type": "beyond", "reference": "custom_signals.llod", "buffer_atr": None},
        prov="presenter_stated",
    )
    log.add("rules", "flod_llod: placeholder entry/target rules -> null; stop_rule rewritten from generic LLOD prose to structured beyond custom_signals.llod")
    s["notes"] = (
        "Framework, not a standalone entry model: it explains how to rank PD arrays inside a retracement. "
        "The higher-probability case is the overlap of a bullish FVG and a bullish order block. "
        "Written on the canonical bullish side; the presenter examples are bearish mirrors."
    )
    log.add("mirror", "flod_llod: presenter examples are bearish; summary, sequence, invalidation and stop were rewritten on the canonical bullish side")
