import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from reg_seed import REG_A, REG_B
from reg_pilot import REG_PILOT
import glob, importlib


def extras():
    out = []
    for p in sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'regx_*.py'))):
        mod = importlib.import_module(os.path.splitext(os.path.basename(p))[0])
        out.extend(getattr(mod, 'REG_EXTRA', []))
    return out


def build(extra=()):
    entries = REG_A + REG_B + REG_PILOT + extras() + list(extra)
    names = [e["name"] for e in entries]
    dup = {n for n in names if names.count(n) > 1}
    assert not dup, dup
    reg = {"registry_version": "1.0.0",
           "conventions": {
               "canonical_side": "bullish/long; apply the catalog mirror table for bearish/short",
               "copy_rule": "strategies copy name, kind, plain_definition, numeric_definition and definition_source verbatim; ts is strategy-specific",
               "level_rule": "kind 'level': if value_expr is null the numeric_definition IS the price; otherwise numeric_definition is the validity condition and value_expr the price",
               "null_numeric": "numeric_definition null => not computable; reason in not_computable_reason"},
           "signals": sorted(entries, key=lambda e: e["name"])}
    os.makedirs("strategies/catalog", exist_ok=True)
    json.dump(reg, open("strategies/catalog/custom_signals.json", "w", encoding="utf-8"), indent=2, ensure_ascii=False)
    print("registry", len(entries))


if __name__ == "__main__":
    build()
