from reg_seed import R


REG_EXTRA = [
    R(
        "measured_swing_projection",
        "level",
        "A Fibonacci projection family drawn from a Judas Swing back to its opening-price anchor; the presenter uses the exact levels -0.27, -0.68, -1.0, -1.62, and -2.0 depending on which one overlaps the intended PD array.",
        None,
        "presenter_stated",
        aliases=["swing_projection", "symmetrical_swing_point", "double_symmetrical_swing_point"],
        value_expr=None,
        notes="The lesson selects among several exact projection levels and allows wick-to-wick or body-to-body anchors across London, daily, weekly, and monthly opening-price profiles. Without one machine-bound opening anchor, one machine-bound Judas-swing leg, and one machine-bound chosen ratio, this signal cannot collapse to a single price.",
        tags=(),
        valid_tfs=None,
        reason="The presenter states a family of exact Fibonacci targets, but the active opening anchor, the wick-vs-body anchor choice, and the chosen target ratio remain discretionary at evaluation time.",
    ),
    R(
        "standard_deviations",
        "level",
        "A Fibonacci projection family for indices and futures that uses exact 0.5 increments: -0.5, -1.0, -1.5, -2.0, -2.5, and -3.0.",
        None,
        "presenter_stated",
        aliases=["standard_deviation_levels", "indices_standard_deviations"],
        value_expr=None,
        notes="The lesson treats these as a menu of targets selected by whichever standard-deviation level overlaps the intended PD array; the active profile anchor and chosen level are not frozen by one engine object.",
        tags=(),
        valid_tfs=None,
        reason="The presenter gives exact ratios, but the signal is a discretionary family of possible targets rather than one single computable price at evaluation time.",
    ),
]


CON_EXTRA = []
