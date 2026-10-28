import json, glob
reg = {e["name"]: e for e in json.load(open("strategies/catalog/custom_signals.json", encoding="utf-8"))["signals"]}
n = 0
for p in sorted(glob.glob("strategies/*.json")):
    d = json.load(open(p, encoding="utf-8"))
    for s in d["strategies"]:
        for k in ("invalidation", "filters"):
            for i, it in enumerate(s[k]):
                if it.get("numeric_definition") is None:
                    n += 1
                    print("%s|%s[%d]|%s" % (s["strategy_id"], k, i, it["text"][:150]))
        for cs in s["custom_signals"]:
            if cs["numeric_definition"] is None:
                print("%s|CS %s|%s" % (s["strategy_id"], cs["name"], reg[cs["name"]].get("not_computable_reason")))
        for r in ("entry_rule", "target_rule", "stop_rule"):
            if s[r]["text"] and s[r]["structured"] is None:
                print("%s|%s text w/o structured|%s" % (s["strategy_id"], r, s[r]["text"][:100]))
print(n)
