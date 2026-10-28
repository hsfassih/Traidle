from reg_seed import R


REG_EXTRA = [
    R(
        "decoupled_market",
        "indicator_relation",
        "The 30-year Treasury bond market and the US Dollar are not giving the normal inverse read: they are moving together, or the bond leg is too compressed to trust for bias.",
        None,
        "presenter_stated",
        aliases=["decoupled"],
        notes="Would need external.bonds.* and external.dxy.* inputs; the transcript also treats 'consolidating heavily' as part of the same low-probability condition without giving a numeric threshold.",
        tags=["market:needs_external_symbols"],
        reason="Requires bond and dollar symbols plus a discretionary 'consolidating heavily' judgement; the video gives no exact measurement window or threshold.",
    ),
    R(
        "relatively_stronger_pair",
        "indicator_relation",
        "Among correlated instruments, this chart is the preferred long when it expands further in the bullish direction or holds a swing low that the peer sweeps; for shorts, mirror it to the relatively weaker pair.",
        None,
        "presenter_stated",
        aliases=["relative_strength_pair", "relative_strength"],
        notes="Would need external.correlated.* inputs and, for basket comparisons such as ES/NQ/YM, more than one peer symbol. The transcript does not fix a numeric lookback or a single strength metric.",
        tags=["market:needs_external_symbols"],
        reason="Requires cross-symbol comparison plus presenter-defined 'more explosive move' / 'fails to break the swing' logic across an arbitrary peer set.",
    ),
]


CON_EXTRA = []
