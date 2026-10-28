"""Independent audit of strategies/*.json against strategies/raw/*.json (not the linter)."""
import glob, json, os, re, sys
sys.path.insert(0, os.path.join("scripts", "strategy_work"))
from corpus import extract

ALLOWED_NEW = {"numeric_definition"}
raw_S, raw_E = {}, {}
for p in sorted(glob.glob("strategies/raw/*.json")):
    d = json.load(open(p, encoding="utf-8"))
    S, E = extract(d)
    for s in S:
        raw_S[s["strategy_id"]] = s
    for e in E:
        raw_E.setdefault(e["example_id"], e)

new_S, new_E = {}, {}
for p in sorted(glob.glob("strategies/*.json")):
    d = json.load(open(p, encoding="utf-8"))
    for s in d["strategies"]:
        new_S[s["strategy_id"]] = s
    for e in d["examples"]:
        new_E[e["example_id"]] = e

problems = []
print("strategies raw/new:", len(raw_S), len(new_S), " examples raw(unique ids)/new:", len(raw_E), len(new_E))
missing = set(raw_S) - set(new_S)
if missing:
    problems.append(f"strategies lost: {missing}")

def keydiff(a, b, path, out):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in a:
            if k not in b:
                out.append(f"{path}.{k} REMOVED")
            else:
                keydiff(a[k], b[k], f"{path}.{k}", out)
        for k in b:
            if k not in a and k not in ALLOWED_NEW:
                out.append(f"{path}.{k} added")
    elif isinstance(a, list) and isinstance(b, list):
        if a and all(isinstance(x, dict) for x in a) and b and all(isinstance(x, dict) for x in b):
            for i, (x, y) in enumerate(zip(a, b)):
                keydiff(x, y, f"{path}[{i}]", out)
    elif type(a) != type(b) and not (a is None or b is None):
        out.append(f"{path} retyped {type(a).__name__}->{type(b).__name__}")

# 1. key preservation (shallow structure; step lists/custom signals legitimately change)
SKIP = {"signals_required", "custom_signals", "glossary", "examples", "uncertainties", "presenter_claims", "reinforces", "conflicts_with", "sequence"}
removed, added, retyped = [], [], []
for sid, rs in raw_S.items():
    ns = new_S.get(sid)
    if not ns:
        continue
    out = []
    for k in rs:
        if k not in ns:
            out.append(f"{k} REMOVED")
        elif k not in SKIP:
            keydiff(rs[k], ns[k], k, out)
    for k in ns:
        if k not in rs:
            out.append(f"{k} added")
    for o in out:
        (removed if "REMOVED" in o else retyped if "retyped" in o else added).append(f"{sid}: {o}")
print("\n[1] strategy key REMOVED:", len(removed)); [print("  ", x) for x in removed[:40]]
print("[1] strategy key retyped:", len(retyped)); [print("  ", x) for x in retyped[:40]]
print("[1] strategy key added:", len(added)); [print("  ", x) for x in added[:40]]

# 2. untouched fields
for sid, rs in raw_S.items():
    ns = new_S.get(sid)
    for k in ("weight", "historical_stats", "confidence"):
        if k in rs and json.dumps(rs[k], sort_keys=True) != json.dumps(ns.get(k), sort_keys=True):
            problems.append(f"{sid}.{k} changed")
    rp, np_ = rs.get("presenter_claims") or [], ns.get("presenter_claims") or []
    if len(rp) != len(np_):
        problems.append(f"{sid}.presenter_claims length {len(rp)}->{len(np_)}")
    if any(c.get("verified") is not False for c in np_):
        problems.append(f"{sid}.presenter_claims verified not all false")
    if any(c.get("verified") is not False for c in rp):
        print("   note: raw already had verified != false in", sid)

# 3. example keys + candles untouched
cand_changed = []
ex_missing = sorted(set(raw_E) - set(new_E))
ex_extra = sorted(set(new_E) - set(raw_E))
for eid, re_ in raw_E.items():
    ne = new_E.get(eid)
    if not ne:
        continue
    out = []
    for k in re_:
        if k not in ne:
            out.append(f"{k} REMOVED")
    if out:
        problems.append(f"example {eid}: " + ", ".join(out))
    rf, nf = re_.get("frames") or [], ne.get("frames") or []
    if len(rf) != len(nf):
        problems.append(f"example {eid}: frames {len(rf)}->{len(nf)}")
    for a, b in zip(rf, nf):
        if json.dumps(a.get("candles"), sort_keys=True) != json.dumps(b.get("candles"), sort_keys=True):
            cand_changed.append(eid)
        for k in a:
            if k not in b:
                problems.append(f"example {eid} frame key {k} REMOVED")
    rt, nt = re_.get("trade") or {}, ne.get("trade") or {}
    for k in ("entry_price", "stop_price", "target_price", "entry", "stop", "target", "prices"):
        if k in rt and json.dumps(rt[k]) != json.dumps(nt.get(k)):
            problems.append(f"example {eid}: trade.{k} changed")
print("\n[3] raw example ids not carried (dropped/not-present in cleaned):", ex_missing)
print("[3] extra example ids:", ex_extra)
print("[3] examples whose candle rows changed:", cand_changed)

# 4. numeric literals used in numeric_definitions (look for invented precision)
lit = {}
def scan(expr, where):
    if not isinstance(expr, str):
        return
    for m in re.findall(r"(?<![A-Za-z_\[\-@\d:.])(\d+\.?\d*)(?![\d:A-Za-z_])", expr):
        lit.setdefault(m, set()).add(where)
reg = json.load(open("strategies/catalog/custom_signals.json", encoding="utf-8"))["signals"]
for e in reg:
    scan(e["numeric_definition"], "reg:" + e["name"]); scan(e.get("value_expr"), "reg:" + e["name"])
for sid, s in new_S.items():
    for k in ("invalidation", "filters"):
        for it in s.get(k) or []:
            scan(it.get("numeric_definition"), f"{sid}.{k}")
print("\n[4] numeric literals in expressions:")
for k in sorted(lit, key=float):
    print("  ", k, "->", sorted(lit[k])[:8])

# 5. forbidden units in numeric_definition text
bad = []
for e in reg:
    for f in ("numeric_definition", "value_expr"):
        v = e.get(f) or ""
        if re.search(r"pip|point|tick|\$|\bUSD", v, re.I):
            bad.append((e["name"], v))
print("\n[5] registry expressions with pips/points/ticks/$:", bad)

# 6. placeholder prose in rules, 'Not explicitly'
for sid, s in new_S.items():
    for k in ("entry_rule", "target_rule", "stop_rule"):
        t = s[k].get("text")
        if isinstance(t, str) and re.match(r"\s*(not (explicitly )?(defined|specified)|no (explicit|specific))", t, re.I):
            problems.append(f"{sid}.{k} placeholder text")
# 7. summaries: at most 2 sentences
for sid, s in new_S.items():
    n = len(re.findall(r"[.!?](?:\s|$)", s["summary"].strip()))
    if n > 2:
        problems.append(f"{sid}.summary has {n} sentences")
# 8. category distribution
from collections import Counter
print("\n[8] categories:", dict(Counter(s["category"] for s in new_S.values())))
print("[8] rule completeness: entry text present:", sum(1 for s in new_S.values() if s["entry_rule"]["text"]),
      " target:", sum(1 for s in new_S.values() if s["target_rule"]["text"]), " stop:", sum(1 for s in new_S.values() if s["stop_rule"]["text"]))
print("\nPROBLEMS:", len(problems))
for p in problems:
    print("  -", p)
