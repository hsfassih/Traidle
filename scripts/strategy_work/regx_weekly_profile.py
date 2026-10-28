from reg_seed import INTRADAY, R


REG_EXTRA = [
    R(
        "friday_tgif_reversal",
        "price_action",
        "On Friday, price sweeps Thursday's low and closes back above it, producing the TGIF reversal; mirror for bearish.",
        "ny_dow == 5 AND low <= pdl AND close >= pdl",
        "presenter_stated",
        aliases=["tgif_reversal", "friday_turtle_soup"],
        valid_tfs=INTRADAY,
        notes="Uses the previous-day low on Friday, so it must be derived from intraday candles aligned to the New York day.",
    ),
]

CON_EXTRA = []
