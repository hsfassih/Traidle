def step(n, signal, op, val, tf, ts=None, optional=False, maxc=None, prov="presenter_stated"):
    custom = signal.startswith("custom_signals.")
    return {"step": n, "signal": signal, op: val, "timeframe": tf, "optional": optional,
            "max_candles_after_previous_step": maxc, "traidle_supported": not custom, "provenance": prov, "ts": ts}


def rule(text, structured, prov, ts, quote):
    return {"text": text, "structured": structured, "provenance": prov, "ts": ts, "quote": quote}


def keep_rule(old, text=None, structured=None, prov=None):
    out = dict(old)
    if text is not None:
        out["text"] = text
    out["structured"] = structured
    if prov is not None:
        out["provenance"] = prov
    return out


def null_rule(old):
    out = dict(old)
    out.update({"text": None, "structured": None, "provenance": None, "ts": None, "quote": None})
    return out


def set_numeric(items, numerics):
    for it, nd in zip(items, numerics):
        it["numeric_definition"] = nd


def strategy(doc, sid):
    return next(s for s in doc["strategies"] if s["strategy_id"] == sid)


def example(doc, eid):
    return next(e for e in doc["examples"] if e["example_id"] == eid)


def remap_walkthrough(e, mapping, log):
    for i, w in enumerate(e["setup_walkthrough"]):
        old = w.get("maps_to_signal_step")
        new = mapping[i] if isinstance(mapping, list) else mapping.get(old, old)
        if new != old:
            log.add("walkthrough_map", f"{e['example_id']}[{i}]: maps_to_signal_step {old} -> {new}")
        w["maps_to_signal_step"] = new
    renumber_walkthrough(e, log)


def renumber_walkthrough(e, log):
    steps = [w.get("step") for w in e["setup_walkthrough"]]
    want = list(range(1, len(steps) + 1))
    if steps != want:
        for w, n in zip(e["setup_walkthrough"], want):
            w["step"] = n
        log.add("walkthrough_renumber", f"{e['example_id']}: walkthrough steps {steps} -> {want}")


def resolve_refs(s, mapping, log):
    for key in ("reinforces", "conflicts_with"):
        out = []
        for x in s.get(key) or []:
            new = mapping.get(x, x if not x.startswith("ref:") else None)
            if new is None:
                log.add("ref_removed", f"{s['strategy_id']}.{key}: '{x}' is a concept, not a strategy; removed")
            else:
                if new != x:
                    log.add("ref_resolved", f"{s['strategy_id']}.{key}: '{x}' -> '{new}'")
                if new not in out:
                    out.append(new)
        s[key] = out


def set_category(s, new, log):
    if s.get("category") != new:
        log.add("category", f"{s['strategy_id']}: '{s.get('category')}' -> '{new}'")
        s["category"] = new


def set_field(s, key, value, log, cat="semantic"):
    if s.get(key) != value:
        log.add(cat, f"{s['strategy_id']}.{key} rewritten")
        s[key] = value


def drop_example(doc, eid, reason, log):
    doc["examples"] = [e for e in doc["examples"] if e["example_id"] != eid]
    for s in doc["strategies"]:
        if eid in (s.get("examples") or []):
            s["examples"] = [x for x in s["examples"] if x != eid]
    log.add("example_dropped", f"{eid}: {reason}")


def drop_uncertainty(s, issue_contains, reason, log):
    before = len(s.get("uncertainties") or [])
    s["uncertainties"] = [u for u in (s.get("uncertainties") or []) if issue_contains.lower() not in (u.get("issue") or "").lower()]
    if len(s["uncertainties"]) != before:
        log.add("uncertainty_dropped", f"{s['strategy_id']}: '{issue_contains}' dropped ({reason})")
