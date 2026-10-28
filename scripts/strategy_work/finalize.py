import json, glob, os, re

REG = "strategies/catalog/custom_signals.json"
CON = "strategies/catalog/constants.json"


def main():
    reg = json.load(open(REG, encoding="utf-8"))
    con = json.load(open(CON, encoding="utf-8"))
    by_name = {e["name"]: e for e in reg["signals"]}
    k_re = re.compile(r"\bK_[A-Z0-9_]+\b")
    reg_k = {n: set(k_re.findall((e.get("numeric_definition") or "") + " " + (e.get("value_expr") or ""))) for n, e in by_name.items()}
    used_sig, used_k = {}, {}
    for p in sorted(glob.glob("strategies/*.json")):
        d = json.load(open(p, encoding="utf-8"))
        if not isinstance(d, dict) or "strategies" not in d:
            continue
        for s in d["strategies"]:
            sid = s["strategy_id"]
            for cs in s.get("custom_signals") or []:
                used_sig.setdefault(cs["name"], set()).add(sid)
                for k in reg_k.get(cs["name"], set()):
                    used_k.setdefault(k, set()).add(sid)
            blob = json.dumps([s.get("invalidation"), s.get("filters"), s.get("entry_rule"), s.get("target_rule"), s.get("stop_rule")])
            for k in k_re.findall(blob):
                used_k.setdefault(k, set()).add(sid)
    for e in reg["signals"]:
        e["used_by"] = sorted(used_sig.get(e["name"], []))
    for c in con["constants"]:
        c["used_by"] = sorted(used_k.get(c["name"], []))
    json.dump(reg, open(REG, "w", encoding="utf-8"), indent=2, ensure_ascii=False)
    json.dump(con, open(CON, "w", encoding="utf-8"), indent=2, ensure_ascii=False)
    print("registry entries used:", sum(1 for e in reg["signals"] if e["used_by"]), "/", len(reg["signals"]))


if __name__ == "__main__":
    main()
