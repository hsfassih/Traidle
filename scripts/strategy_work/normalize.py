import json, os, re, sys, importlib, copy
sys.path.insert(0, os.path.dirname(__file__))
from corpus import extract

RAW = "strategies/raw"
OUT = "strategies"
LOG_PATH = "scripts/strategy_work/changelog.json"

ASR = [
    (r"\b[Vv]erify [Gg]aps\b", "fair value gaps"), (r"\b[Vv]erif(?:y|ied) [Gg]ap\b", "fair value gap"),
    (r"\b(?:the smallest little )?verified Gap\b", "fair value gap"), (r"\bverify again\b", "fair value gap"),
    (r"\bFairfax app\b", "fair value gap"), (r"\b[Ff]orefather [Gg]ap\b", "fair value gap"),
    (r"\b[Ff]arewell [Gg]ap\b", "fair value gap"), (r"\b[Ff]errari [Gg]ap\b", "fair value gap"),
    (r"\b[Ff]ry [Gg]ap\b", "fair value gap"), (r"\b[Ss]urvey [Gg]ap\b", "fair value gap"),
    (r"\b[Rr]evive [Gg]ap\b", "fair value gap"), (r"\bfor Value Gap\b", "fair value gap"),
    (r"\bfor fire gap\b", "fair value gap"), (r"\bfor Vega\b", "fair value gap"), (r"\bfi gap\b", "fair value gap"),
    (r"\bbullish fagot\b", "bullish fair value gap"), (r"\bvery very Gap\b", "fair value gap"),
    (r"\bSD entry\b", "ST entry"), (r"\bthat is our SD\b", "that is our ST"),
    (r"\bDrone liquidity\b", "draw on liquidity"), (r"\bdrawn liquidity\b", "draw on liquidity"),
    (r"\bdraw liquidity\b", "draw on liquidity"), (r"\bor block\b", "order block"), (r"\bMagnus\b", "magnet"),
    (r"\b[Pp]rice action lag\b", lambda m: m.group(0)[:-3] + "leg"), (r"\bbeat array\b", "PD array"),
    (r"\bBD arrays\b", "PD arrays"), (r"\bPD Rays\b", "PD arrays"), (r"\bPD Ray\b", "PD array"),
    (r"\bfair value cups\b", "fair value gaps"), (r"\bLondon clothes\b", "London close"),
    (r"\bpremium race\b", "premium array"), (r"\bdiscount race\b", "discount arrays"),
]
TF_MAP = {"1D": "1d", "Daily": "1d", "daily": "1d", "1H": "1h", "1 hour": "1h", "4 hour": "4h", "4H": "4h",
          "1W": "1w", "Weekly": "1w", "1M": "1mo", "Monthly": "1mo", "4H and 5m": "4h|5m", "unknown": None}
TZ_MAP = {"New York": "America/New_York", "EST": "America/New_York", "NY": "America/New_York"}
KZ_MAP = {"news": "unknown", "new_york_pm": "none"}
DETECT_MAP = {"bearish_fvg": ["fvg.bearish"], "bullish_fvg": ["fvg.bullish"], "fvg_mitigation": ["fvg.mitigated"],
              "swing_high": ["swing_high"], "swing_low": ["swing_low"], "order_blocks": ["order_block"],
              "swing_points": ["swing_high", "swing_low"], "swing_point": ["swing_high", "swing_low"],
              "liquidity_sweep_low": ["structural_event.liquidity_sweep_low"],
              "liquidity_sweep_high": ["structural_event.liquidity_sweep_high"],
              "bullish_choch": ["structural_event.bullish_choch"], "bearish_choch": ["structural_event.bearish_choch"],
              "bullish_bos": ["structural_event.bullish_bos"], "bearish_bos": ["structural_event.bearish_bos"],
              "fvg on 15m": ["fvg"], "liquidity_sweep_high on 5m": ["structural_event.liquidity_sweep_high"],
              "swing_point.is_high": ["swing_high"]}
CATALOG = json.load(open("strategies/catalog/signal_catalog.json", encoding="utf-8"))
DETECTIONS = set(CATALOG["detections"])
REGISTRY = {e["name"]: e for e in json.load(open("strategies/catalog/custom_signals.json", encoding="utf-8"))["signals"]}
ALIASES = {a: n for n, e in REGISTRY.items() for a in e.get("aliases") or []}
MAY_DIFFER_MAP = {"order_block.invalidation": "order_block.invalidated",
                  "order_block.mitigation": "custom_signals.swing_failure_pattern",
                  "custom_signals.order_block.mitigation": "custom_signals.swing_failure_pattern"}


class Log:
    def __init__(self, fname):
        self.fname = fname
        self.items = []

    def add(self, cat, detail):
        self.items.append({"file": self.fname, "category": cat, "detail": detail})


def walk_strings(node, fn, path=""):
    if isinstance(node, dict):
        for k in list(node.keys()):
            node[k] = walk_strings(node[k], fn, path + "." + k)
        return node
    if isinstance(node, list):
        return [walk_strings(v, fn, path + "[]") for v in node]
    if isinstance(node, str):
        return fn(node, path)
    return node


def fix_ts_value(v, key_path, log, video_max_s):
    if not isinstance(v, str):
        return v
    orig = v
    if "-" in v and re.fullmatch(r"[\d:]+-[\d:]+", v):
        v = v.split("-")[0]
        log.add("ts", f"{key_path}: range '{orig}' -> start '{v}'")
    m = re.fullmatch(r"(?:(\d{1,2}):)?(\d{1,2}):(\d{2})", v)
    if not m:
        return v
    total = int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
    if total >= 3600:
        if video_max_s and total > video_max_s:
            log.add("ts_flagged", f"{key_path}: impossible '{orig}' (video length ~{video_max_s // 60} min) -> null")
            return "__IMPOSSIBLE__" + orig
        return f"{total // 3600:02d}:{total % 3600 // 60:02d}:{total % 60:02d}"
    new = f"{total // 60:02d}:{total % 60:02d}"
    if new != orig and "range" not in orig:
        log.add("ts_format", f"{key_path}: '{orig}' -> '{new}'")
    return new


def normalize_ts(node, log, video_max_s, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            p = path + "." + k
            if k in ("ts", "start", "end", "canonical_frame_ts") and isinstance(v, str):
                node[k] = fix_ts_value(v, p, log, video_max_s)
            else:
                normalize_ts(v, log, video_max_s, p)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            normalize_ts(v, log, video_max_s, path + f"[{i}]")


def mech_strings(doc, log):
    counts = {}

    def fn(s, path):
        if s in ("null", "N/A", "n/a", "", "None"):
            log.add("placeholder_null", f"{path}: '{s}' -> null")
            return None
        new = s
        for pat, rep in ASR:
            new2 = re.sub(pat, rep, new)
            if new2 != new:
                counts[pat] = counts.get(pat, 0) + len(re.findall(pat, new))
                new = new2
        return new

    doc = walk_strings(doc, fn)
    for pat, n in counts.items():
        log.add("asr", f"{pat} x{n}")
    return doc


def mech_example(e, log):
    eid = e.get("example_id")
    for fr in e.get("frames") or []:
        tf = fr.get("timeframe")
        if tf in TF_MAP:
            fr["timeframe"] = TF_MAP[tf]
            log.add("example_enum", f"{eid}: frame timeframe '{tf}' -> {json.dumps(TF_MAP[tf])}")
        tz = fr.get("chart_timezone")
        if tz in TZ_MAP:
            fr["chart_timezone"] = TZ_MAP[tz]
            log.add("example_enum", f"{eid}: chart_timezone '{tz}' -> '{TZ_MAP[tz]}'")
        so = fr.get("signals_observed") or {}
        kz = so.get("kill_zone")
        if kz in KZ_MAP:
            so["kill_zone"] = KZ_MAP[kz]
            log.add("example_enum", f"{eid}: kill_zone '{kz}' -> '{KZ_MAP[kz]}'")
    t = e.get("trade") or {}
    if t.get("direction") == "both":
        t["direction"] = None
        log.add("example_enum", f"{eid}: trade.direction 'both' -> null")
    if t.get("outcome") == "explanation_only":
        t["outcome"] = "not_taken"
        log.add("example_enum", f"{eid}: trade.outcome 'explanation_only' -> 'not_taken'")
    ec = e.get("engine_check") or {}
    for md in ec.get("may_differ") or []:
        sig = md.get("signal")
        if sig in MAY_DIFFER_MAP:
            md["signal"] = MAY_DIFFER_MAP[sig]
            log.add("legacy_signal", f"{eid}: may_differ '{sig}' -> '{MAY_DIFFER_MAP[sig]}'")
        elif isinstance(sig, str) and sig.startswith("custom_signals.") and sig[15:] in ALIASES:
            md["signal"] = "custom_signals." + ALIASES[sig[15:]]
            log.add("custom_alias", f"{eid}: may_differ '{sig}' -> canonical '{md['signal']}'")
    for fr in e.get("frames") or []:
        for cso in fr.get("custom_signals_observed") or []:
            n = cso.get("name")
            if n not in REGISTRY and n in ALIASES:
                cso["name"] = ALIASES[n]
                log.add("custom_alias", f"{eid}: custom_signals_observed '{n}' -> canonical '{ALIASES[n]}'")
    new_sd, moved = [], []
    for x in ec.get("should_detect") or []:
        if x in DETECTIONS:
            new_sd.append(x)
        elif x in DETECT_MAP:
            new_sd.extend(y for y in DETECT_MAP[x] if y not in new_sd)
            log.add("legacy_signal", f"{eid}: engine_check '{x}' -> {DETECT_MAP[x]}")
        elif x.startswith("structural_event.") and x.split(".", 1)[1] in DETECT_MAP:
            new_sd.extend(DETECT_MAP[x.split(".", 1)[1]])
        else:
            moved.append(x)
    if moved:
        for x in moved:
            target = MAY_DIFFER_MAP.get(x, x if x.startswith("custom_signals.") else "custom_signals." + x)
            if target.startswith("custom_signals.") and target[15:] in ALIASES:
                target = "custom_signals." + ALIASES[target[15:]]
            ec.setdefault("may_differ", []).append(
                {"signal": target, "reason": "Custom concept, not an engine detection; moved from should_detect."})
            log.add("engine_check", f"{eid}: '{x}' moved from should_detect to may_differ as '{target}' (not an engine detection)")
    if "should_detect" in ec:
        ec["should_detect"] = new_sd


def build(fname, semantic=True):
    log = Log(fname)
    raw = json.load(open(os.path.join(RAW, fname), encoding="utf-8"))
    text = open(os.path.join(RAW, fname), encoding="utf-8").read()
    indent = len(re.search(r"\n( +)\S", text).group(1))
    S_raw, E_raw = extract(copy.deepcopy(raw))
    shape = "list" if isinstance(raw, list) else ",".join(raw.keys())[:80]
    log.add("envelope", f"shape [{shape}] -> {{strategies, examples}}")
    strategies, seen_ids = [], set()
    for s in S_raw:
        for k in ("chart_examples", "pass_3_chart_examples"):
            if k in s:
                del s[k]
                log.add("envelope", f"{s['strategy_id']}: nested '{k}' moved to top-level examples")
        if isinstance(s.get("examples"), list) and any(isinstance(x, dict) for x in s["examples"]):
            s["examples"] = [x["example_id"] if isinstance(x, dict) else x for x in s["examples"]]
            log.add("envelope", f"{s['strategy_id']}: full example objects in 'examples' moved to top level")
        strategies.append(s)
    sids = {s["strategy_id"] for s in strategies}
    examples = []
    for e in E_raw:
        eid = e.get("example_id")
        if eid in seen_ids:
            log.add("example_dedup", f"{eid}: duplicate copy dropped")
            continue
        if e.get("strategy_id") not in sids:
            log.add("example_dropped", f"{eid}: strategy_id '{e.get('strategy_id')}' does not resolve in this file; not carried")
            continue
        seen_ids.add(eid)
        examples.append(e)
    for s in strategies:
        own = [e["example_id"] for e in examples if e["strategy_id"] == s["strategy_id"]]
        listed = s.get("examples") or []
        missing = [x for x in listed if x not in own]
        if missing:
            log.add("example_missing", f"{s['strategy_id']}: listed but absent {missing}")
        added = [x for x in own if x not in listed]
        if added:
            log.add("example_linked", f"{s['strategy_id']}: owned examples added to list {added}")
        s["examples"] = [x for x in listed if x in own] + added
    doc = {"strategies": strategies, "examples": examples}
    doc = mech_strings(doc, log)
    vmax = video_max_seconds(raw)
    normalize_ts(doc, log, vmax)
    for e in doc["examples"]:
        mech_example(e, log)
    if semantic:
        mod_name = "sem_" + os.path.splitext(fname)[0]
        if os.path.exists(os.path.join(os.path.dirname(__file__), mod_name + ".py")):
            mod = importlib.import_module(mod_name)
            mod.apply(doc, log, Ctx(log))
    impossible_to_null(doc, log)
    return doc, indent, log


def video_max_seconds(raw):
    vals = []

    def w(n):
        if isinstance(n, dict):
            for k, v in n.items():
                if k in ("ts", "start", "end", "canonical_frame_ts") and isinstance(v, str):
                    for part in v.split("-"):
                        m = re.fullmatch(r"(?:(\d{1,2}):)?(\d{1,2}):(\d{2})", part)
                        if m:
                            vals.append(int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + int(m.group(3)))
                else:
                    w(v)
        elif isinstance(n, list):
            for v in n:
                w(v)
    w(raw)
    vals.sort()
    if not vals:
        return None
    body = [v for v in vals if v < 3600]
    return max(body) if len(body) >= 0.9 * len(vals) else None


def impossible_to_null(doc, log):
    def w(n, found):
        if isinstance(n, dict):
            for k, v in n.items():
                if isinstance(v, str) and v.startswith("__IMPOSSIBLE__"):
                    n[k] = None
                    found.append(v[len("__IMPOSSIBLE__"):])
                else:
                    w(v, found)
        elif isinstance(n, list):
            for v in n:
                w(v, found)
    for obj in doc["strategies"] + doc["examples"]:
        found = []
        w(obj, found)
        for orig in found:
            obj.setdefault("uncertainties", []).append({
                "issue": f"Impossible timestamp {orig}",
                "why": "The value exceeds the video length; it was set to null during normalization instead of guessing the intended time.",
                "ts": None})

class Ctx:
    def __init__(self, log):
        self.log = log

    def cs(self, name, ts=None):
        r = REGISTRY[name]
        return {"name": name, "kind": r["kind"], "plain_definition": r["plain_definition"],
                "numeric_definition": r["numeric_definition"], "definition_source": r["definition_source"], "ts": ts}


def dumps(obj, indent, level=0, key=None):
    pad, inner = " " * (indent * level), " " * (indent * (level + 1))
    if isinstance(obj, dict):
        if not obj:
            return "{}"
        items = [f"{inner}{json.dumps(k, ensure_ascii=False)}: {dumps(v, indent, level + 1, k)}" for k, v in obj.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(obj, list):
        if not obj:
            return "[]"
        if key == "candles":
            return "[\n" + ",\n".join(inner + dumps(v, indent, level + 1, "row") for v in obj) + "\n" + pad + "]"
        if key in ("row", "low_confidence_candles", "candle_i_range"):
            return "[" + ", ".join(json.dumps(v, ensure_ascii=False) for v in obj) + "]"
        items = [inner + dumps(v, indent, level + 1, None) for v in obj]
        return "[\n" + ",\n".join(items) + "\n" + pad + "]"
    return json.dumps(obj, ensure_ascii=False)


def run(files, semantic=True):
    all_logs = json.load(open(LOG_PATH, encoding="utf-8")) if os.path.exists(LOG_PATH) else {}
    for fname in files:
        doc, indent, log = build(fname, semantic)
        with open(os.path.join(OUT, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write(dumps(doc, indent) + "\n")
        all_logs[fname] = log.items
        print(fname, "->", len(log.items), "log entries")
    json.dump(all_logs, open(LOG_PATH, "w", encoding="utf-8"), indent=1, ensure_ascii=False)


if __name__ == "__main__":
    run(sys.argv[1:])
