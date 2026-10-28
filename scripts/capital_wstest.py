#!/usr/bin/env python3
"""
capital_api_test.py - step-by-step diagnostic for the Capital.com REST + WebSocket API (for Traidle).

Reads the same .env as forex_ws_benchmark.py (put it next to this script, or pass --env-file):
  CAPITAL_API_KEY       your API key
  CAPITAL_IDENTIFIER    your Capital.com login e-mail
  CAPITAL_PASSWORD      the custom password you chose when you created the API key (NOT your login password)
  CAPITAL_ENV           live | demo   (optional; the script tries the other one if login fails)

Install:  pip install websockets requests
Run:      python capital_api_test.py
          python capital_api_test.py --duration 700            # long enough to cross the 10-minute session expiry
          python capital_api_test.py --duration 700 --ping-every 0   # prove what happens WITHOUT keepalive pings
          python capital_api_test.py --epics EURUSD,GBPUSD,USDJPY,XAUUSD --conns 5

What it checks (each step prints PASS / WARN / FAIL, a summary is printed at the end):
  1  login (REST session) incl. live/demo auto-detect and a plain-language reason when it fails
  2  server time + estimate of your PC clock offset (so latency numbers are meaningful)
  3  market status and snapshot price for every epic (is the market open?)
  4  REST history for every resolution (candle count per call, depth, volume present?)
  5  WebSocket: connect, ping RTT, quote stream, native OHLC stream, tick rate, gaps, latency
  6  native candles vs REST candles vs candles you build yourself from quote ticks (accuracy)
  7  several WebSocket connections with the same session at once
  8  session still valid afterwards (keepalive / expiry behaviour)
Forex is closed on weekends: you then get a market-status line but no ticks.
"""
import argparse
import asyncio
import json
import os
import sys
import time
from datetime import datetime, timezone

try:
    import requests
    import websockets
except ImportError:
    sys.exit("pip install websockets requests")

REST_LIVE = "https://api-capital.backend-capital.com"
REST_DEMO = "https://demo-api-capital.backend-capital.com"
WS_DEFAULT = "wss://api-streaming-capital.backend-capital.com/connect"
RESOLUTIONS = ["MINUTE", "MINUTE_5", "MINUTE_15", "MINUTE_30", "HOUR", "HOUR_4", "DAY", "WEEK"]

RESULTS = []  # (status, step, text)


def report(status, step, text):
    RESULTS.append((status, step, text))
    print(f"  [{status:4s}] {text}")


def head(n, title):
    print(f"\n=== {n}. {title}")


def load_env_file(path):
    if not os.path.isfile(path):
        return 0
    n = 0
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            if line.startswith("export "):
                line = line[7:].strip()
            k, _, v = line.partition("=")
            k, v = k.strip(), v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
                v = v[1:-1]
            else:
                v = v.split(" #", 1)[0].strip()
            if k and k not in os.environ:
                os.environ[k] = v
                n += 1
    return n


def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    return s[min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))]


def f0(v):
    return "-" if v is None else f"{v:.0f}"


def mask(s):
    return (s[:4] + "..." + s[-3:]) if s and len(s) > 8 else "***"


def iso_utc_ms(s):
    dt = datetime.fromisoformat(s.replace("Z", ""))
    return dt.replace(tzinfo=timezone.utc).timestamp() * 1000.0


# ------------------------------------------------------------------ REST

class Capital:
    def __init__(self, key, ident, pw, base):
        self.key, self.ident, self.pw, self.base = key, ident, pw, base
        self.cst = self.sec = None
        self.session_body = {}
        self.s = requests.Session()

    def login(self):
        t = time.monotonic()
        r = self.s.post(f"{self.base}/api/v1/session", timeout=20,
                        headers={"X-CAP-API-KEY": self.key, "Content-Type": "application/json"},
                        json={"identifier": self.ident, "password": self.pw, "encryptedPassword": False})
        ms = (time.monotonic() - t) * 1000
        if r.status_code == 200:
            self.cst, self.sec = r.headers.get("CST"), r.headers.get("X-SECURITY-TOKEN")
            try:
                self.session_body = r.json()
            except Exception:
                self.session_body = {}
        return r, ms

    def get(self, path, **params):
        t = time.monotonic()
        r = self.s.get(f"{self.base}/api/v1{path}", params=params or None, timeout=20,
                       headers={"CST": self.cst or "", "X-SECURITY-TOKEN": self.sec or ""})
        return r, (time.monotonic() - t) * 1000


def explain_login_error(r):
    try:
        code = r.json().get("errorCode", "")
    except Exception:
        code = r.text[:120]
    hints = {
        "error.invalid.api.key": "API key not recognised on this environment. Check CAPITAL_API_KEY; a key created on "
                                 "a demo account only works on demo (set CAPITAL_ENV=demo); make sure the key is enabled.",
        "error.invalid.details": "e-mail or password rejected. CAPITAL_PASSWORD must be the CUSTOM PASSWORD set while "
                                 "creating the API key, not your normal login password. Check CAPITAL_IDENTIFIER too.",
        "error.null.api.key": "no API key was sent (CAPITAL_API_KEY empty).",
        "error.too-many.requests": "rate limited (1 session request per second). Wait a few seconds and retry.",
    }
    return code, hints.get(code, "see the errorCode above; also check 2FA / account verification on capital.com")


def step_login(args):
    head(1, "Login (REST session)")
    key, ident, pw = (os.environ.get(k) for k in ("CAPITAL_API_KEY", "CAPITAL_IDENTIFIER", "CAPITAL_PASSWORD"))
    missing = [n for n, v in (("CAPITAL_API_KEY", key), ("CAPITAL_IDENTIFIER", ident), ("CAPITAL_PASSWORD", pw)) if not v]
    if missing:
        report("FAIL", "login", "missing in .env/environment: " + ", ".join(missing))
        return None
    report("INFO", "login", f"key={mask(key)} identifier={ident} password=({len(pw)} chars)")
    override = os.environ.get("FX_CAPITAL_REST")
    order = [override] if override else ([REST_DEMO, REST_LIVE] if (os.environ.get("CAPITAL_ENV", "live").lower() == "demo")
                                           else [REST_LIVE, REST_DEMO])
    for base in order:
        cap = Capital(key, ident, pw, base)
        try:
            r, ms = cap.login()
        except Exception as e:
            report("WARN", "login", f"{base}: {e!r}")
            continue
        env = "DEMO" if "demo" in base else "LIVE"
        if r.status_code == 200:
            body = cap.session_body
            report("PASS", "login", f"logged in on {env} in {ms:.0f} ms (CST {mask(cap.cst)}, security token {mask(cap.sec)})")
            report("INFO", "login", f"account id={body.get('currentAccountId') or body.get('accountId')} "
                                    f"streamingHost={body.get('streamingHost')} accounts={len(body.get('accounts', []) or [])}")
            return cap
        code, hint = explain_login_error(r)
        report("WARN", "login", f"{env}: HTTP {r.status_code} {code}")
        report("INFO", "login", "hint: " + hint)
        time.sleep(1.2)  # session endpoint allows 1 request per second
    report("FAIL", "login", "could not log in on live or demo - fix the credentials above and rerun")
    return None


def step_time(cap):
    head(2, "Server time / PC clock offset")
    try:
        t0 = time.time() * 1000
        r, ms = cap.get("/time")
        t1 = time.time() * 1000
        if r.status_code != 200:
            report("WARN", "time", f"/time returned HTTP {r.status_code}; latency numbers will include your clock error")
            return 0.0
        st = r.json().get("serverTime")
        offset = (t0 + t1) / 2.0 - st  # positive = your clock is ahead of the server
        report("INFO", "time", f"REST round-trip {ms:.0f} ms; your PC clock is ahead of Capital.com by ~{offset:.0f} ms "
                               f"(+-{(t1 - t0) / 2:.0f} ms)")
        if abs(offset) > 1000:
            report("WARN", "time", "your clock is more than 1 s off; sync it (the script corrects for it below)")
        else:
            report("PASS", "time", "clock offset is small")
        return offset
    except Exception as e:
        report("WARN", "time", f"could not read server time: {e!r}")
        return 0.0


def step_markets(cap, epics):
    head(3, "Markets (is the epic valid and open?)")
    open_any = False
    for e in epics:
        try:
            r, ms = cap.get(f"/markets/{e}")
        except Exception as ex:
            report("FAIL", "markets", f"{e}: {ex!r}")
            continue
        if r.status_code != 200:
            report("FAIL", "markets", f"{e}: HTTP {r.status_code} {r.text[:100]}")
            continue
        b = r.json()
        snap, ins = b.get("snapshot", {}), b.get("instrument", {})
        status = snap.get("marketStatus")
        open_any |= status == "TRADEABLE"
        report("PASS" if status == "TRADEABLE" else "WARN", "markets",
               f"{e}: {ins.get('name')} status={status} bid={snap.get('bid')} offer={snap.get('offer')} "
               f"updateTime={snap.get('updateTime')} ({ms:.0f} ms)")
    if not open_any:
        report("WARN", "markets", "no market is TRADEABLE right now (weekend/holiday?) - the streaming test will be silent")
    return open_any


def step_history(cap, epic):
    head(4, f"REST history ({epic})")
    out = {}
    for res in RESOLUTIONS:
        try:
            r, ms = cap.get(f"/prices/{epic}", resolution=res, max=1000)
        except Exception as ex:
            report("FAIL", "history", f"{res}: {ex!r}")
            continue
        if r.status_code != 200:
            report("FAIL", "history", f"{res}: HTTP {r.status_code} {r.text[:100]}")
            continue
        pr = r.json().get("prices", [])
        vols = [p.get("lastTradedVolume") or 0 for p in pr]
        first = pr[0].get("snapshotTimeUTC") if pr else None
        last = pr[-1].get("snapshotTimeUTC") if pr else None
        out[res] = len(pr)
        report("PASS" if pr else "WARN", "history",
               f"{res:10s} {len(pr):4d} candles {first} -> {last}  volume>0 in {sum(1 for v in vols if v)}/{len(pr)}  ({ms:.0f} ms)")
        time.sleep(0.15)  # stay far below 10 requests/second
    if out:
        report("INFO", "history", "volume here is tick volume for forex, not exchange volume; "
                                  f"max candles per call observed: {max(out.values())}")
    return out


# ------------------------------------------------------------------ WebSocket

class WsRun:
    def __init__(self):
        self.connect_ms = None
        self.ack = []
        self.quotes = []     # (recv_ms, epic, bid, ofr, ts)
        self.candles = []    # (recv_ms, epic, res, t, o, h, l, c)
        self.ping_rtts = []
        self.errors = []
        self.close = None
        self.other = []
        self.first_quote_ms = None
        self.first_candle_ms = None


def ws_msg(cst, sec, dest, cid, payload=None):
    m = {"destination": dest, "correlationId": str(cid), "cst": cst, "securityToken": sec}
    if payload is not None:
        m["payload"] = payload
    return json.dumps(m)


async def ws_client(url, cap, epics, resolutions, duration, ping_every, run, with_ohlc=True):
    t = time.monotonic()
    try:
        ws = await websockets.connect(url, max_size=None, open_timeout=15, ping_interval=20)
    except Exception as e:
        run.errors.append(f"connect: {e!r}")
        return
    run.connect_ms = (time.monotonic() - t) * 1000
    t0 = time.monotonic()
    deadline = t0 + duration
    ping_at = {}
    cid = 0

    async def send(dest, payload=None):
        nonlocal cid
        cid += 1
        if dest == "ping":
            ping_at[str(cid)] = time.monotonic()
        await ws.send(ws_msg(cap.cst, cap.sec, dest, cid, payload))

    await send("ping")
    await send("marketData.subscribe", {"epics": epics})
    if with_ohlc:
        await send("OHLCMarketData.subscribe", {"epics": epics, "resolutions": resolutions, "type": "classic"})
    last_ping = time.monotonic()
    while time.monotonic() < deadline:
        if ping_every and time.monotonic() - last_ping >= ping_every:
            last_ping = time.monotonic()
            await send("ping")
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=min(1.0, max(0.05, deadline - time.monotonic())))
        except asyncio.TimeoutError:
            continue
        except websockets.ConnectionClosed as e:
            rc = getattr(e, "rcvd", None)
            run.close = f"{rc.code} {rc.reason}" if rc else repr(e)
            run.close += f" after {time.monotonic() - t0:.0f}s"
            return
        now = time.time() * 1000
        try:
            m = json.loads(raw)
        except Exception:
            continue
        d, p = m.get("destination"), m.get("payload") or {}
        if m.get("status") not in (None, "OK"):
            run.errors.append(str(m)[:200])
        if d == "quote":
            if run.first_quote_ms is None:
                run.first_quote_ms = (time.monotonic() - t0) * 1000
            run.quotes.append((now, p.get("epic"), p.get("bid"), p.get("ofr"), p.get("timestamp")))
        elif d == "ohlc.event":
            if run.first_candle_ms is None:
                run.first_candle_ms = (time.monotonic() - t0) * 1000
            run.candles.append((now, p.get("epic"), p.get("resolution"), p.get("t"),
                                p.get("o"), p.get("h"), p.get("l"), p.get("c")))
        elif d == "ping":
            k = str(m.get("correlationId"))
            if k in ping_at:
                run.ping_rtts.append((time.monotonic() - ping_at[k]) * 1000)
        else:
            if len(run.ack) < 6:
                run.ack.append(str(m)[:260])
    try:
        await ws.close()
    except Exception:
        pass


def analyse_quotes(run, offset_ms, duration):
    per = {}
    for r, e, b, o, ts in run.quotes:
        per.setdefault(e, []).append((r, b, o, ts))
    for e, rows in per.items():
        gaps = [rows[i][0] - rows[i - 1][0] for i in range(1, len(rows))]
        lat = [r - ts - offset_ms for r, _, _, ts in rows if ts]
        spread = [(o - b) for _, b, o, _ in rows if b is not None and o is not None]
        report("INFO", "ws", f"{e}: {len(rows)} quotes ({len(rows) / duration:.2f}/s) gap p50/p95/max "
                             f"{f0(pct(gaps, 50))}/{f0(pct(gaps, 95))}/{f0(max(gaps) if gaps else None)} ms, "
                             f"latency min/p50/p95 {f0(pct(lat, 0))}/{f0(pct(lat, 50))}/{f0(pct(lat, 95))} ms "
                             f"(clock-corrected), spread p50 {pct(spread, 50)}")


def analyse_candles(run, duration):
    by = {}
    for r, e, res, t, o, h, l, c in run.candles:
        by.setdefault((e, res), []).append(r)
    for (e, res), times in sorted(by.items(), key=lambda kv: (str(kv[0][0]), str(kv[0][1]))):
        gaps = [times[i] - times[i - 1] for i in range(1, len(times))]
        report("INFO", "ws", f"candle {e} {res}: {len(times)} updates, interval p50/p95 "
                             f"{f0(pct(gaps, 50))}/{f0(pct(gaps, 95))} ms")


def final_ws_candles(run, res):
    """last update per (epic, candle start) = final values; also returns newest start per epic."""
    fin, newest = {}, {}
    for _, e, r, t, o, h, l, c in run.candles:
        if r != res or t is None:
            continue
        fin[(e, t)] = (o, h, l, c)
        newest[e] = max(newest.get(e, 0), t)
    return fin, newest


def local_candles_from_quotes(run, res_ms=60000):
    d = {}
    for _, e, b, o, ts in run.quotes:
        if b is None or not ts:
            continue
        k = (e, int(ts // res_ms) * res_ms)
        c = d.get(k)
        if not c:
            d[k] = [b, b, b, b]
        else:
            c[1], c[2], c[3] = max(c[1], b), min(c[2], b), b
    return d


def step_compare(cap, run, epics):
    head(6, "Native candles vs REST candles vs candles built from quote ticks (MINUTE, bid side)")
    fin, newest = final_ws_candles(run, "MINUTE")
    if not fin:
        report("WARN", "compare", "no native MINUTE candle events were received")
        return
    local = local_candles_from_quotes(run)
    for e in epics:
        done = [(t, v) for (ee, t), v in fin.items() if ee == e and t < newest.get(e, 0)]
        if not done:
            report("WARN", "compare", f"{e}: no completed minute inside the run (use --duration 180 or more)")
            continue
        rest = {}
        try:
            r, _ = cap.get(f"/prices/{e}", resolution="MINUTE", max=60)
            for p in r.json().get("prices", []):
                rest[int(iso_utc_ms(p["snapshotTimeUTC"]))] = (p["openPrice"]["bid"], p["highPrice"]["bid"],
                                                                p["lowPrice"]["bid"], p["closePrice"]["bid"])
        except Exception as ex:
            report("WARN", "compare", f"{e}: REST history failed: {ex!r}")
        d_rest, n_rest, d_loc, n_loc = 0.0, 0, 0.0, 0
        signed, d_rl = [], 0.0   # signed native-minus-REST differences; max |REST - ticks-built|
        for t, (o, h, l, c) in done:
            if int(t) in rest:
                n_rest += 1
                d_rest = max(d_rest, max(abs(a - b) for a, b in zip((o, h, l, c), rest[int(t)])))
                signed += [a - b for a, b in zip((o, h, l, c), rest[int(t)])]
                if (e, int(t)) in local and t != min(tt for tt, _ in done):
                    d_rl = max(d_rl, max(abs(a - b) for a, b in zip(rest[int(t)], local[(e, int(t))])))
            lk = (e, int(t))
            if lk in local:
                n_loc += 1
                # the first partial minute at start-up is incomplete by design, so skip the oldest candle
                if t != min(tt for tt, _ in done):
                    d_loc = max(d_loc, max(abs(a - b) for a, b in zip((o, h, l, c), local[lk])))
        report("PASS" if n_rest and d_rest < 1e-4 else "WARN", "compare",
               f"{e}: {len(done)} completed native minutes; vs REST: {n_rest} matched, max |diff| {d_rest:.6f}; "
               f"vs ticks-built: {n_loc} matched, max |diff| {d_loc:.6f}")
        if signed:
            report("INFO", "compare", f"{e}: mean(native - REST bid) = {sum(signed) / len(signed):+.6f} "
                                      f"(about +spread means native candles are ask-side, about 0 means bid-side); "
                                      f"max |REST - ticks-built| = {d_rl:.6f}")
    report("INFO", "compare", "small ticks-built differences are normal (you miss ticks the feed does not send); "
                              "if they are tiny, building every Traidle timeframe locally from quotes is safe")


async def step_ws(cap, args, epics, offset_ms, ws_url):
    head(5, f"WebSocket stream ({args.duration}s, ping every {args.ping_every or 'never'}s)")
    run = WsRun()
    await ws_client(ws_url, cap, epics, args.resolutions, args.duration, args.ping_every, run)
    if run.connect_ms is None:
        report("FAIL", "ws", "WebSocket connection failed: " + "; ".join(run.errors))
        return run
    report("PASS", "ws", f"connected in {run.connect_ms:.0f} ms; ping RTT "
                         f"{', '.join(f'{x:.0f}' for x in run.ping_rtts[:5]) or 'no reply'} ms")
    for a in run.ack:
        report("INFO", "ws", "server: " + a)
    if run.errors:
        report("FAIL", "ws", "server errors: " + " | ".join(run.errors[:3]))
    if run.close:
        report("WARN" if args.ping_every == 0 else "FAIL", "ws", f"server closed the socket: {run.close}")
    if run.quotes:
        report("PASS", "ws", f"{len(run.quotes)} quotes; first quote after {run.first_quote_ms:.0f} ms")
        analyse_quotes(run, offset_ms, args.duration)
    else:
        report("WARN", "ws", "no quotes received (market closed, or subscription refused - see server lines above)")
    if run.candles:
        report("PASS", "ws", f"{len(run.candles)} native OHLC events; first after {run.first_candle_ms:.0f} ms")
        analyse_candles(run, args.duration)
    else:
        report("WARN", "ws", "no native OHLC events received")
    return run


async def step_conns(cap, args, epics, ws_url):
    head(7, f"{args.conns} simultaneous WebSocket connections (same session tokens, {args.conn_seconds}s)")
    runs = [WsRun() for _ in range(args.conns)]
    await asyncio.gather(*[ws_client(ws_url, cap, epics, ["MINUTE"], args.conn_seconds, 0, r, with_ohlc=False) for r in runs])
    ok = sum(1 for r in runs if r.connect_ms is not None)
    rx = sum(1 for r in runs if r.quotes)
    closed = [r.close for r in runs if r.close]
    errs = sorted({e for r in runs for e in r.errors})[:3]
    status = "PASS" if rx == args.conns else ("WARN" if rx else "FAIL")
    report(status, "conns", f"requested {args.conns}, connected {ok}, receiving quotes {rx}, closed by server {len(closed)}")
    for c in closed[:3]:
        report("INFO", "conns", "closed: " + str(c))
    for e in errs:
        report("INFO", "conns", "error: " + e)


def step_session_alive(cap, args):
    head(8, "Session still valid after the streaming run?")
    try:
        r, ms = cap.get("/session")
        if r.status_code == 200:
            report("PASS", "session", f"session still valid ({ms:.0f} ms)")
        else:
            report("FAIL" if args.ping_every else "WARN", "session",
                   f"HTTP {r.status_code} {r.text[:100]} - session expired (re-login needed)")
    except Exception as e:
        report("WARN", "session", repr(e))


def summary():
    print("\n================ SUMMARY ================")
    for s in ("FAIL", "WARN", "PASS"):
        items = [x for x in RESULTS if x[0] == s]
        print(f"{s}: {len(items)}")
    for st, step, text in RESULTS:
        if st in ("FAIL", "WARN"):
            print(f"  [{st}] {step}: {text}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--epics", default="EURUSD,GBPUSD,USDJPY")
    ap.add_argument("--duration", type=int, default=180, help="seconds of streaming (>=180 to finish a few 1m candles)")
    ap.add_argument("--resolutions", default="MINUTE,MINUTE_5,HOUR", type=lambda s: [x.strip() for x in s.split(",")])
    ap.add_argument("--ping-every", type=int, default=300, help="seconds between keepalive pings (0 = never)")
    ap.add_argument("--conns", type=int, default=3, help="simultaneous sockets for step 7 (0 = skip)")
    ap.add_argument("--conn-seconds", type=int, default=20)
    ap.add_argument("--skip-history", action="store_true")
    ap.add_argument("--out", default="capital_test_results.json")
    ap.add_argument("--env-file", default=None)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    env_path = args.env_file or next((p for p in (os.path.join(here, ".env"), os.path.join(os.getcwd(), ".env"))
                                      if os.path.isfile(p)), os.path.join(here, ".env"))
    n = load_env_file(env_path)
    print(f"Loaded {n} variable(s) from {env_path}" if os.path.isfile(env_path) else f"No .env at {env_path}")
    epics = [e.strip().upper() for e in args.epics.split(",")]

    cap = step_login(args)
    if not cap:
        summary()
        sys.exit(1)
    offset = step_time(cap)
    step_markets(cap, epics)
    if not args.skip_history:
        step_history(cap, epics[0])
    ws_url = os.environ.get("FX_CAPITAL_URL")
    if not ws_url:
        host = (cap.session_body or {}).get("streamingHost")
        ws_url = (host.rstrip("/") + "/connect") if host and host.startswith(("wss://", "ws://")) else WS_DEFAULT
    print(f"\n(WebSocket endpoint: {ws_url})")
    run = asyncio.run(step_ws(cap, args, epics, offset, ws_url))
    step_compare(cap, run, epics)
    if args.conns > 1:
        asyncio.run(step_conns(cap, args, epics, ws_url))
    step_session_alive(cap, args)
    summary()
    with open(args.out, "w") as f:
        json.dump({"when": datetime.now(timezone.utc).isoformat(), "epics": epics, "duration_s": args.duration,
                   "results": [{"status": s, "step": st, "text": t} for s, st, t in RESULTS]}, f, indent=2)
    print(f"\nSaved {args.out}")


if __name__ == "__main__":
    main()