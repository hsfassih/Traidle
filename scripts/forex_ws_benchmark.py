#!/usr/bin/env python3
"""
forex_ws_benchmark.py - benchmark free Forex WebSocket feeds for Traidle.

Providers: Capital.com, EODHD, Tiingo, Twelve Data, Finnhub
Optional : TraderMade (TRADERMADE_API_KEY), OANDA v20 practice (OANDA_TOKEN + OANDA_ACCOUNT)

Keys are read from environment variables (nothing is hard-coded):
  CAPITAL_API_KEY  CAPITAL_IDENTIFIER (login e-mail)  CAPITAL_PASSWORD (the *API key custom password*)
  CAPITAL_ENV=live|demo   (default: live)
  EODHD_API_KEY  TIINGO_API_KEY  TWELVEDATA_API_KEY  FINNHUB_API_KEY
  TRADERMADE_API_KEY (optional)  OANDA_TOKEN / OANDA_ACCOUNT (optional)

Install:  pip install websockets requests
Run:      python forex_ws_benchmark.py --duration 90
          python forex_ws_benchmark.py --providers tiingo,capital --duration 300 --conn-test 3

What it measures per provider (all providers run at the same time on the same clock):
  connect_ms     TCP + TLS + WebSocket handshake
  ready_ms       handshake -> auth / subscribe acknowledged
  first_tick_ms  handshake -> first price tick
  ticks, ticks/s per symbol, inter-arrival gaps (median / p95 / max)
  latency        local receive time minus the provider's own timestamp (needs NTP-synced PC clock;
                 'jitter' = p95 - p50 is independent of clock offset)
  fields         which fields arrive (bid / ask / mid / volume / native OHLC)
  candles        native OHLC events (Capital.com) or locally built 1-minute candles from ticks
  --conn-test N  opens N parallel sockets with the same key and reports how many work
"""
import argparse
import asyncio
import json
import os
import statistics
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets requests")
try:
    import requests
except ImportError:
    sys.exit("pip install websockets requests")

# ----------------------------------------------------------------------------- helpers

def load_env_file(path: str) -> int:
    """Minimal .env loader (no extra dependency). Existing environment variables win over the file.
    Accepts KEY=VALUE, optional 'export ' prefix, quoted values, and # comments."""
    if not os.path.isfile(path):
        return 0
    loaded = 0
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            if line.startswith("export "):
                line = line[len("export "):].strip()
            key, _, val = line.partition("=")
            key, val = key.strip(), val.strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
                val = val[1:-1]
            else:
                val = val.split(" #", 1)[0].strip()  # inline comment for unquoted values
            if key and key not in os.environ:
                os.environ[key] = val
                loaded += 1
    return loaded


def now_ms() -> float:
    return time.time() * 1000.0


def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    k = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[k]


def parse_iso_ms(s: str):
    """Tolerant ISO-8601 parser ('2026-01-01T10:00:00.123456789+00:00' / '...Z') -> epoch ms."""
    try:
        s = s.strip().replace("Z", "+00:00")
        if "." in s:
            head, rest = s.split(".", 1)
            digits = ""
            for ch in rest:
                if ch.isdigit():
                    digits += ch
                else:
                    break
            tail = rest[len(digits):]
            s = f"{head}.{digits[:6].ljust(6, '0')}{tail}"
        dt = datetime.fromisoformat(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.timestamp() * 1000.0
    except Exception:
        return None


def url_override(name: str, default: str) -> str:
    return os.environ.get(f"FX_{name.upper()}_URL", default)


@dataclass
class Rec:
    name: str
    native_ohlc: bool = False
    t0: float = 0.0                       # monotonic, set when handshake finished
    connect_ms: float = None
    ready_ms: float = None
    first_tick_ms: float = None
    ticks: list = field(default_factory=list)    # (recv_wall_ms, sym, bid, ask, prov_ms)
    candles: list = field(default_factory=list)  # native OHLC events
    fields: set = field(default_factory=set)
    other: list = field(default_factory=list)    # first non-tick messages (for diagnosis)
    errors: list = field(default_factory=list)
    close: str = None
    ping_rtt_ms: float = None
    note: str = ""

    def log_other(self, raw):
        if len(self.other) < 6:
            self.other.append(str(raw)[:240])

    def tick(self, sym, bid=None, ask=None, prov_ms=None):
        r = now_ms()
        if self.first_tick_ms is None:
            self.first_tick_ms = (time.monotonic() - self.t0) * 1000.0
        self.ticks.append((r, sym, bid, ask, prov_ms))


class LocalCandles:
    """Builds 1-minute OHLC candles from ticks (mid price) - what Traidle must do for tick-only feeds."""
    def __init__(self):
        self.cur = {}
        self.done = []

    def update(self, sym, ts_ms, bid, ask):
        px = None
        if bid is not None and ask is not None:
            px = (bid + ask) / 2.0
        else:
            px = bid if bid is not None else ask
        if px is None:
            return
        minute = int(ts_ms // 60000)
        c = self.cur.get(sym)
        if c and c["m"] != minute:
            self.done.append((sym, c))
            c = None
        if not c:
            c = {"m": minute, "o": px, "h": px, "l": px, "c": px, "n": 0}
            self.cur[sym] = c
        c["h"] = max(c["h"], px)
        c["l"] = min(c["l"], px)
        c["c"] = px
        c["n"] += 1


async def pump(ws, rec, on_msg, deadline):
    while time.monotonic() < deadline:
        left = deadline - time.monotonic()
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=max(0.05, left))
        except asyncio.TimeoutError:
            break
        except websockets.ConnectionClosed as e:
            rcvd = getattr(e, "rcvd", None)
            rec.close = (f"{rcvd.code} {rcvd.reason}" if rcvd else repr(e)).strip()
            break
        try:
            on_msg(raw)
        except Exception as e:  # never let a parser bug kill the run
            rec.errors.append(f"parse: {e!r} on {str(raw)[:120]}")


async def open_ws(rec, url):
    t = time.monotonic()
    ws = await websockets.connect(url, max_size=None, open_timeout=15, ping_interval=20)
    rec.connect_ms = (time.monotonic() - t) * 1000.0
    rec.t0 = time.monotonic()
    return ws


def jloads(raw):
    try:
        return json.loads(raw)
    except Exception:
        return None

# ----------------------------------------------------------------------------- providers
# Each adapter: async def run_<name>(rec, symbols, deadline)
# `symbols` are plain 6-letter pairs such as EURUSD.

def fx_slash(s):  return f"{s[:3]}/{s[3:]}"
def fx_under(s):  return f"{s[:3]}_{s[3:]}"


async def run_capital(rec, symbols, deadline):
    rec.native_ohlc = True
    key = os.environ.get("CAPITAL_API_KEY")
    ident = os.environ.get("CAPITAL_IDENTIFIER")
    pw = os.environ.get("CAPITAL_PASSWORD")
    if not (key and ident and pw):
        rec.errors.append("need CAPITAL_API_KEY, CAPITAL_IDENTIFIER, CAPITAL_PASSWORD")
        return
    env = os.environ.get("CAPITAL_ENV", "live").lower()
    base = os.environ.get("FX_CAPITAL_REST") or (
        "https://demo-api-capital.backend-capital.com" if env == "demo"
        else "https://api-capital.backend-capital.com")
    t = time.monotonic()
    try:
        r = await asyncio.to_thread(
            requests.post, f"{base}/api/v1/session",
            headers={"X-CAP-API-KEY": key, "Content-Type": "application/json"},
            json={"identifier": ident, "password": pw, "encryptedPassword": False}, timeout=15)
    except Exception as e:
        rec.errors.append(f"session: {e!r}")
        return
    rec.note = f"REST session {int((time.monotonic()-t)*1000)} ms"
    if r.status_code != 200:
        rec.errors.append(f"session HTTP {r.status_code}: {r.text[:160]}")
        return
    cst, sec = r.headers.get("CST"), r.headers.get("X-SECURITY-TOKEN")
    ws_url = url_override("capital", "wss://api-streaming-capital.backend-capital.com/connect")
    ws = await open_ws(rec, ws_url)
    ping_sent = {}

    def send(dest, cid, payload=None):
        m = {"destination": dest, "correlationId": str(cid), "cst": cst, "securityToken": sec}
        if payload is not None:
            m["payload"] = payload
        return ws.send(json.dumps(m))

    epics = list(symbols)
    ping_sent["1"] = time.monotonic()
    await send("ping", 1)
    await send("marketData.subscribe", 2, {"epics": epics})
    await send("OHLCMarketData.subscribe", 3,
               {"epics": epics, "resolutions": ["MINUTE", "MINUTE_5"], "type": "classic"})
    last_ping = time.monotonic()

    def on_msg(raw):
        m = jloads(raw)
        if not isinstance(m, dict):
            return rec.log_other(raw)
        d = m.get("destination")
        p = m.get("payload") or {}
        if d == "quote":
            rec.fields.update(["bid", "ask"])
            rec.tick(p.get("epic"), p.get("bid"), p.get("ofr"), p.get("timestamp"))
        elif d == "ohlc.event":
            rec.fields.update(["ohlc(bid-side)"])
            rec.candles.append((now_ms(), p.get("epic"), p.get("resolution"), p.get("t"),
                                p.get("o"), p.get("h"), p.get("l"), p.get("c")))
        elif d == "ping":
            if rec.ping_rtt_ms is None and "1" in ping_sent:
                rec.ping_rtt_ms = (time.monotonic() - ping_sent["1"]) * 1000.0
            rec.log_other(raw)
        else:
            if rec.ready_ms is None and m.get("status") == "OK":
                rec.ready_ms = (time.monotonic() - rec.t0) * 1000.0
            rec.log_other(raw)

    # keepalive: session dies after 10 min without activity -> ping every 5 min
    async def keepalive():
        nonlocal last_ping
        while time.monotonic() < deadline:
            await asyncio.sleep(1)
            if time.monotonic() - last_ping > 300:
                last_ping = time.monotonic()
                await send("ping", 99)
    ka = asyncio.create_task(keepalive())
    try:
        await pump(ws, rec, on_msg, deadline)
    finally:
        ka.cancel()
        await ws.close()


async def run_eodhd(rec, symbols, deadline):
    key = os.environ.get("EODHD_API_KEY")
    if not key:
        rec.errors.append("need EODHD_API_KEY")
        return
    ws = await open_ws(rec, url_override("eodhd", f"wss://ws.eodhistoricaldata.com/ws/forex?api_token={key}"))
    await ws.send(json.dumps({"action": "subscribe", "symbols": ",".join(symbols)}))  # comma STRING, not array

    def on_msg(raw):
        m = jloads(raw)
        if not isinstance(m, dict):
            return rec.log_other(raw)
        if "s" in m and ("a" in m or "b" in m):
            rec.fields.update(["bid", "ask"])
            rec.tick(m["s"], m.get("b"), m.get("a"), m.get("t"))
        else:
            if rec.ready_ms is None:
                rec.ready_ms = (time.monotonic() - rec.t0) * 1000.0
            if m.get("status_code", 200) >= 400 or "error" in str(m).lower():
                rec.errors.append(str(m)[:200])
            rec.log_other(raw)
    await pump(ws, rec, on_msg, deadline)
    await ws.close()


async def run_tiingo(rec, symbols, deadline):
    key = os.environ.get("TIINGO_API_KEY")
    if not key:
        rec.errors.append("need TIINGO_API_KEY")
        return
    ws = await open_ws(rec, url_override("tiingo", "wss://api.tiingo.com/fx"))
    await ws.send(json.dumps({"eventName": "subscribe", "authorization": key,
                              "eventData": {"thresholdLevel": 5,
                                            "tickers": [s.lower() for s in symbols]}}))

    def on_msg(raw):
        m = jloads(raw)
        if not isinstance(m, dict):
            return rec.log_other(raw)
        mt = m.get("messageType")
        if mt == "A" and isinstance(m.get("data"), list) and m["data"] and m["data"][0] == "Q":
            d = m["data"]
            rec.fields.update(["bid", "ask", "mid", "bid_size", "ask_size"])
            rec.tick(str(d[1]).upper(), d[4], d[7], parse_iso_ms(str(d[2])))
        elif mt == "H":
            pass
        else:
            if mt == "I" and rec.ready_ms is None:
                rec.ready_ms = (time.monotonic() - rec.t0) * 1000.0
            if mt == "E" or (m.get("response") or {}).get("code", 200) >= 400:
                rec.errors.append(str(m)[:200])
            rec.log_other(raw)
    await pump(ws, rec, on_msg, deadline)
    await ws.close()


async def run_twelvedata(rec, symbols, deadline):
    key = os.environ.get("TWELVEDATA_API_KEY")
    if not key:
        rec.errors.append("need TWELVEDATA_API_KEY")
        return
    ws = await open_ws(rec, url_override("twelvedata", f"wss://ws.twelvedata.com/v1/quotes/price?apikey={key}"))
    await ws.send(json.dumps({"action": "subscribe", "params": {"symbols": ",".join(fx_slash(s) for s in symbols)}}))

    async def hb():
        while time.monotonic() < deadline:
            await asyncio.sleep(10)
            try:
                await ws.send(json.dumps({"action": "heartbeat"}))
            except Exception:
                return
    h = asyncio.create_task(hb())

    def on_msg(raw):
        m = jloads(raw)
        if not isinstance(m, dict):
            return rec.log_other(raw)
        ev = m.get("event")
        if ev == "price":
            rec.fields.add("price")
            for k in ("bid", "ask"):
                if m.get(k) is not None:
                    rec.fields.add(k)
            if m.get("day_volume") is not None:
                rec.fields.add("day_volume")
            ts = m.get("timestamp")
            rec.tick(str(m.get("symbol", "")).replace("/", ""), m.get("bid", m.get("price")),
                     m.get("ask", m.get("price")), ts * 1000.0 if ts else None)
        else:
            if ev == "subscribe-status":
                rec.ready_ms = (time.monotonic() - rec.t0) * 1000.0
                if m.get("fails"):
                    rec.errors.append("subscribe fails: " + json.dumps(m["fails"])[:200])
            rec.log_other(raw)
    try:
        await pump(ws, rec, on_msg, deadline)
    finally:
        h.cancel()
        await ws.close()


async def run_finnhub(rec, symbols, deadline):
    key = os.environ.get("FINNHUB_API_KEY")
    if not key:
        rec.errors.append("need FINNHUB_API_KEY")
        return
    ws = await open_ws(rec, url_override("finnhub", f"wss://ws.finnhub.io?token={key}"))
    for s in symbols:
        await ws.send(json.dumps({"type": "subscribe", "symbol": f"OANDA:{fx_under(s)}"}))
    rec.ready_ms = (time.monotonic() - rec.t0) * 1000.0  # no explicit ack in the protocol

    def on_msg(raw):
        m = jloads(raw)
        if not isinstance(m, dict):
            return rec.log_other(raw)
        if m.get("type") == "trade":
            for d in m.get("data", []):
                rec.fields.update(["price", "volume"])
                sym = str(d.get("s", "")).split(":")[-1].replace("_", "")
                rec.tick(sym, d.get("p"), d.get("p"), d.get("t"))
        elif m.get("type") == "error":
            rec.errors.append(str(m)[:200])
        else:
            rec.log_other(raw)
    await pump(ws, rec, on_msg, deadline)
    await ws.close()


async def run_tradermade(rec, symbols, deadline):
    key = os.environ.get("TRADERMADE_API_KEY")
    if not key:
        rec.errors.append("need TRADERMADE_API_KEY")
        return
    ws = await open_ws(rec, url_override("tradermade", "wss://stream.tradermade.com/feedAdv"))
    await ws.send(json.dumps({"action": "login", "key": key, "fmt": "JSON"}))

    def on_msg(raw):
        m = jloads(raw)
        if not isinstance(m, dict):
            return rec.log_other(raw)
        t = m.get("t")
        if m.get("s") and t in ("QUOTE", "LAST_QUOTE"):
            rec.fields.update(["bid", "ask", "bid_vol", "ask_vol"])
            rec.tick(m["s"], m.get("b"), m.get("a"), m.get("ts"))
        else:
            rec.log_other(raw)
            if "login_reject" in str(m):
                rec.errors.append(str(m)[:200])
    # login -> subscribe after login_ok
    login_ok = False
    while time.monotonic() < deadline and not login_ok:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
        except Exception as e:
            rec.errors.append(f"login: {e!r}")
            return
        rec.log_other(raw)
        if "login_ok" in str(raw):
            login_ok = True
            rec.ready_ms = (time.monotonic() - rec.t0) * 1000.0
        elif "login_reject" in str(raw):
            rec.errors.append(str(raw)[:200])
            return
    await ws.send(json.dumps({"action": "subscribe", "symbols": [f"{s}:QUOTE" for s in symbols]}))
    await pump(ws, rec, on_msg, deadline)
    await ws.close()


async def run_oanda(rec, symbols, deadline):
    """OANDA v20 pricing stream is chunked HTTP (not WebSocket); at most 4 prices/s/instrument."""
    tok, acc = os.environ.get("OANDA_TOKEN"), os.environ.get("OANDA_ACCOUNT")
    if not (tok and acc):
        rec.errors.append("need OANDA_TOKEN and OANDA_ACCOUNT")
        return
    host = os.environ.get("OANDA_STREAM_HOST", "stream-fxpractice.oanda.com")
    url = f"https://{host}/v3/accounts/{acc}/pricing/stream?instruments=" + ",".join(fx_under(s) for s in symbols)
    loop = asyncio.get_running_loop()
    q = asyncio.Queue()

    def reader():
        try:
            t = time.monotonic()
            with requests.get(url, headers={"Authorization": f"Bearer {tok}"}, stream=True, timeout=(15, 30)) as r:
                loop.call_soon_threadsafe(q.put_nowait, ("connect", (time.monotonic() - t) * 1000.0, r.status_code))
                if r.status_code != 200:
                    loop.call_soon_threadsafe(q.put_nowait, ("err", r.text[:200], None))
                    return
                for line in r.iter_lines():
                    if time.monotonic() > deadline:
                        return
                    if line:
                        loop.call_soon_threadsafe(q.put_nowait, ("line", line, None))
        except Exception as e:
            loop.call_soon_threadsafe(q.put_nowait, ("err", repr(e), None))
    th = asyncio.create_task(asyncio.to_thread(reader))
    rec.t0 = time.monotonic()
    try:
        while time.monotonic() < deadline:
            try:
                kind, a, b = await asyncio.wait_for(q.get(), timeout=max(0.05, deadline - time.monotonic()))
            except asyncio.TimeoutError:
                break
            if kind == "connect":
                rec.connect_ms = a
                rec.ready_ms = a
            elif kind == "err":
                rec.errors.append(str(a))
                break
            else:
                m = jloads(a)
                if m and m.get("type") == "PRICE":
                    bids, asks = m.get("bids") or [{}], m.get("asks") or [{}]
                    rec.fields.update(["bid", "ask", "liquidity"])
                    rec.tick(m["instrument"].replace("_", ""), float(bids[0].get("price", 0)) or None,
                             float(asks[0].get("price", 0)) or None, parse_iso_ms(m.get("time", "")))
    finally:
        th.cancel()


PROVIDERS = {
    "capital": run_capital,
    "eodhd": run_eodhd,
    "tiingo": run_tiingo,
    "twelvedata": run_twelvedata,
    "finnhub": run_finnhub,
    "tradermade": run_tradermade,
    "oanda": run_oanda,
}
DEFAULT_SET = ["capital", "eodhd", "tiingo", "twelvedata", "finnhub"]

# ----------------------------------------------------------------------------- reporting

def summarize(rec: Rec, symbols, duration):
    ticks = rec.ticks
    out = {"provider": rec.name, "connect_ms": rec.connect_ms, "ready_ms": rec.ready_ms,
           "first_tick_ms": rec.first_tick_ms, "ticks": len(ticks), "native_ohlc": rec.native_ohlc,
           "fields": sorted(rec.fields), "errors": rec.errors[:4], "close": rec.close,
           "ping_rtt_ms": rec.ping_rtt_ms, "note": rec.note, "other_msgs": rec.other}
    secs = max(1e-6, duration)
    out["ticks_per_s_total"] = len(ticks) / secs
    per_sym = {}
    for _, s, *_ in ticks:
        per_sym[s] = per_sym.get(s, 0) + 1
    out["per_symbol"] = {k: round(v / secs, 3) for k, v in per_sym.items()}
    gaps = []
    last = {}
    for r, s, *_ in ticks:
        if s in last:
            gaps.append(r - last[s])
        last[s] = r
    out["gap_ms_p50"], out["gap_ms_p95"], out["gap_ms_max"] = pct(gaps, 50), pct(gaps, 95), (max(gaps) if gaps else None)
    lats = [r - p for r, _, _, _, p in ticks if p]
    out["lat_ms_min"], out["lat_ms_p50"], out["lat_ms_p95"] = pct(lats, 0), pct(lats, 50), pct(lats, 95)
    out["jitter_ms"] = (out["lat_ms_p95"] - out["lat_ms_p50"]) if lats else None
    # timestamps with whole-second resolution (e.g. Twelve Data) make 'lat' fuzzy by up to +-1000 ms
    provs = [p for *_, p in ticks if p]
    out["ts_whole_seconds_only"] = bool(provs) and all(int(p) % 1000 == 0 for p in provs)
    # candles
    if rec.native_ohlc:
        out["native_candle_events"] = len(rec.candles)
        res = {}
        for c in rec.candles:
            res[c[2]] = res.get(c[2], 0) + 1
        out["native_candle_events_by_resolution"] = res
    lc = LocalCandles()
    for r, s, b, a, p in ticks:
        lc.update(s, p or r, b, a)
    out["local_1m_candles_completed"] = len(lc.done)
    out["has_provider_timestamp"] = bool(lats)
    return out


def fmt(v, nd=0):
    if v is None:
        return "-"
    return f"{v:.{nd}f}"


def print_table(rows):
    cols = [("provider", "provider"), ("connect", "connect_ms"), ("ready", "ready_ms"), ("1st tick", "first_tick_ms"),
            ("ticks", "ticks"), ("t/s", "ticks_per_s_total"), ("gap p50", "gap_ms_p50"), ("gap p95", "gap_ms_p95"),
            ("gap max", "gap_ms_max"), ("lat min", "lat_ms_min"), ("lat p50", "lat_ms_p50"), ("jitter", "jitter_ms")]
    widths = [max(len(h), 10) for h, _ in cols]
    line = " | ".join(h.ljust(w) for (h, _), w in zip(cols, widths))
    print("\n" + line)
    print("-" * len(line))
    for r in rows:
        cells = []
        for (h, k), w in zip(cols, widths):
            v = r.get(k)
            cells.append((v if isinstance(v, str) else fmt(v, 1 if k == "ticks_per_s_total" else 0)).ljust(w))
        print(" | ".join(cells))
    print("\n(ms unless stated; connect/ready/1st tick measured from socket open; "
          "lat = local receive time - provider timestamp, needs an NTP-synced clock)\n")
    for r in rows:
        print(f"[{r['provider']}] fields={','.join(r['fields']) or '-'}  native_ohlc={r['native_ohlc']}  "
              f"local_1m_candles={r['local_1m_candles_completed']}  per_symbol_t/s={r['per_symbol']}")
        if r.get("native_candle_events") is not None:
            print(f"   native candle events: {r['native_candle_events']} {r['native_candle_events_by_resolution']}")
        if r.get("ping_rtt_ms") is not None:
            print(f"   ping RTT: {r['ping_rtt_ms']:.0f} ms")
        if r.get("ts_whole_seconds_only"):
            print("   WARNING: provider timestamps have 1-second resolution -> 'lat' columns are only +-1s accurate "
                  "(use gap/tick-rate for this provider)")
        if r["note"]:
            print(f"   note: {r['note']}")
        if r["close"]:
            print(f"   closed by server: {r['close']}")
        for e in r["errors"]:
            print(f"   ERROR: {e}")
        if not r["ticks"] and r["other_msgs"]:
            print("   first non-tick messages (diagnosis):")
            for m in r["other_msgs"]:
                print("     ", m)


async def run_one(name, symbols, deadline, rec):
    try:
        await PROVIDERS[name](rec, symbols, deadline)
    except Exception as e:
        rec.errors.append(f"{type(e).__name__}: {e}")


async def conn_test(name, symbols, n, seconds):
    """Open n sockets with the same key at once: how many connect, how many receive data."""
    deadline = time.monotonic() + seconds
    recs = [Rec(name) for _ in range(n)]
    syms = [[symbols[i % len(symbols)]] for i in range(n)]
    await asyncio.gather(*[run_one(name, syms[i], deadline, recs[i]) for i in range(n)])
    connected = sum(1 for r in recs if r.connect_ms is not None)
    receiving = sum(1 for r in recs if r.ticks)
    closed = [r.close for r in recs if r.close]
    errs = sorted({e for r in recs for e in r.errors})[:3]
    return {"provider": name, "requested": n, "connected": connected, "receiving_ticks": receiving,
            "server_closed": closed, "errors": errs}


async def main_async(args):
    names = [p.strip() for p in args.providers.split(",")] if args.providers != "all" else list(PROVIDERS)
    for n in names:
        if n not in PROVIDERS:
            sys.exit(f"unknown provider {n}; choose from {','.join(PROVIDERS)}")
    symbols = [s.strip().upper().replace("/", "") for s in args.pairs.split(",")]
    print(f"Testing {names} on {symbols} for {args.duration}s (all providers in parallel) ...")
    deadline = time.monotonic() + args.duration
    recs = {n: Rec(n) for n in names}
    t_start = time.monotonic()
    await asyncio.gather(*[run_one(n, symbols, deadline, recs[n]) for n in names])
    elapsed = time.monotonic() - t_start
    rows = [summarize(recs[n], symbols, elapsed) for n in names]
    print_table(rows)
    result = {"when": datetime.now(timezone.utc).isoformat(), "pairs": symbols, "duration_s": elapsed, "stream": rows}
    if args.conn_test > 1:
        print(f"Connection test: {args.conn_test} parallel sockets per provider ...")
        ct = []
        for n in names:
            r = await conn_test(n, symbols, args.conn_test, args.conn_seconds)
            ct.append(r)
            print(f"  {n:11s} requested={r['requested']} connected={r['connected']} "
                  f"receiving={r['receiving_ticks']} closed={r['server_closed']} errors={r['errors']}")
        result["conn_test"] = ct
    with open(args.out, "w") as f:
        json.dump(result, f, indent=2, default=str)
    print(f"\nFull results saved to {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--providers", default=",".join(DEFAULT_SET),
                    help="comma list or 'all' (adds tradermade, oanda). default: the five you supplied")
    ap.add_argument("--pairs", default="EURUSD,GBPUSD,USDJPY")
    ap.add_argument("--duration", type=int, default=90, help="seconds of streaming (use >=300 to also cover 1m/5m candles)")
    ap.add_argument("--conn-test", type=int, default=0, help="also open N parallel sockets per provider (e.g. 3)")
    ap.add_argument("--conn-seconds", type=int, default=20)
    ap.add_argument("--out", default="forex_bench_results.json")
    ap.add_argument("--env-file", default=None,
                    help="path to .env (default: .env next to this script, then ./.env)")
    args = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    env_path = args.env_file or next(
        (p for p in (os.path.join(here, ".env"), os.path.join(os.getcwd(), ".env")) if os.path.isfile(p)),
        os.path.join(here, ".env"))
    n = load_env_file(env_path)
    print(f"Loaded {n} variable(s) from {env_path}" if os.path.isfile(env_path)
          else f"No .env found at {env_path} - using environment variables only")
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()