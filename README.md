# TRAIDLE
Custom chart for trading that allows you to have mutliple tabs, each with indicators, ICT concepts, SMC and AI integration to aid in your analysis.

## Live candles, indicators and charts

A C++17 console application that streams live candles for one instrument across any number of
timeframes (`1m` ... `1M`), writes every closed candle with its indicators to a CSV, computes
SMC/ICT structures, and serves everything to the browser chart in `web/visualizer.html`.

Two markets are supported; you choose one **in the browser**, after the app is running:

| Market | Source | Instrument you enter | Data folder |
|---|---|---|---|
| Binance USD-M Futures (crypto) | public WebSocket, no key needed | symbol, e.g. `BTCUSDT` | `btcusdt/` |
| Forex / CFD via Capital.com | live quotes, **free account + API key** | epic, e.g. `EURUSD`, `XAUUSD` | `fx_eurusd/` |

The app itself no longer asks anything on the terminal. Open `web/visualizer.html`: it shows a
market picker, then a symbol + timeframes form, then the familiar chart with a **Disconnect**
button at the top right. Pressing Disconnect stops that symbol's feed and returns you to the
market picker, so you can switch symbols (or markets) without restarting the container - see
[Switching symbols without restarting](#switching-symbols-without-restarting) below.

<!-- ### Prerequisites

- CMake 3.16+
- A C++17 compiler (MSVC via Visual Studio Build Tools, or another supported Windows toolchain)
- [vcpkg](https://github.com/microsoft/vcpkg), used to fetch Boost (Asio/Beast), OpenSSL, and
  nlohmann-json declared in `vcpkg.json`

### Build

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
``` -->

### Run

Building the project locally would be a headache for you, so just pull the image from GitHub Container Registry and run it from its own dedicated folder:

```powershell
docker pull ghcr.io/hsfassih/traidle:latest
mkdir traidle-run
cd traidle-run
docker run --rm -p 8765:8765 -v ${PWD}:/app/data ghcr.io/hsfassih/traidle:latest
```

This keeps the app's working data (the candle CSVs) in a clean, dedicated directory.
`-p 8765:8765` publishes the chart's WebSocket port so `web/visualizer.html` can connect from your
browser. Note there is no `-it`: the app never reads from the terminal anymore, so it does not
need (or want) an interactive TTY - everything happens in the browser. If binding port 8765 fails
(e.g. something else is already using it), the app now prints the reason and exits immediately
rather than continuing with no way to reach it; free the port, or publish a different one with
`-p <host-port>:8765` and use that port number in the Port field of `web/visualizer.html`.

Open `web/visualizer.html` in your browser (just double-click it, or `file://` it directly - no
web server needed). It connects to the app on port 8765 and shows a market picker:

```
Select market:  [1] Binance USD-M Futures   [2] Forex/CFD via Capital.com
```

### Binance USD-M Futures

Pick Binance, then enter the symbol and timeframes in the form:

```
Symbol (e.g. BTCUSDT): BTCUSDT
Candlestick timeframes, comma-separated (e.g. 1m,15m,1h,1d): 1m,15m,1h
```

Closed candles are appended to `btcusdt/<timeframe>.csv`, and missing history (up to 5 years) is
back-filled from Binance's REST API on start-up.

### Forex / CFD via Capital.com

Capital.com provides the live Forex/CFD quotes. You need a Capital.com account (a free **demo**
account works) and an API key:

1. In Capital.com, turn on two-factor authentication, then go to **Settings → API integrations**
   and **Generate new key**.
2. When asked, give the key a **custom password**. That password, *not* your account password, is
   what Traidle uses as `CAPITAL_PASSWORD`.
3. Put the three values in a file called `.env` in your run folder (`traidle-run`):

   ```
   CAPITAL_API_KEY=your-api-key
   CAPITAL_IDENTIFIER=you@example.com
   CAPITAL_PASSWORD=the-custom-password-of-the-key
   # CAPITAL_ENV=demo      # optional: use the demo account's endpoints (default: live)
   ```

4. Run with the file passed in:

   ```powershell
   docker run --rm --env-file .env -p 8765:8765 -v ${PWD}:/app/data ghcr.io/hsfassih/traidle:latest
   ```

   Docker's `--env-file` keeps quotes literally, so write the values **without quotes**.
   (Without `--env-file`, Traidle also reads a `.env` in its working directory itself and does
   understand quotes; variables that are already set in the environment always win. The file is
   read fresh each time you start a Capital.com session from the browser, so you can edit `.env`
   and use the Disconnect button to pick it up again without restarting the container.)

In `web/visualizer.html`'s market picker, choose **Forex/CFD via Capital.com**, then enter an
epic and the timeframes in the form:

```
Epic (e.g. EURUSD, GBPUSD, USDJPY, XAUUSD): EURUSD
Candlestick timeframes, comma-separated (e.g. 1m,15m,1h,1d): 1m,15m,1h,4h,1d
```

What you get:

- Closed candles go to `fx_<epic>/<timeframe>.csv` (e.g. `fx_eurusd/one_minute.csv`), in the same
  column layout as the Binance CSVs, so everything downstream works unchanged.
- All 15 timeframes are supported: `1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w 1M`. Capital.com
  has no native `3m/2h/6h/8h/12h/3d/1M` data, so those are built locally from the next-smaller
  resolution (history) and from the live quotes (live) with the same bucket maths.
- Candles are built from the **bid** price, bucketed in **UTC** by each quote's own timestamp.
- **Volume is the number of price updates (ticks)**: Forex has no real traded volume. Treat
  volume-based indicators (VWAP, OBV) as activity measures.
- History is back-filled on start-up from Capital.com's REST API (up to 5 years, but capped at
  150,000 base candles per timeframe: about 3.5 months of `1m`). A restart only fetches what is
  missing. Forex is closed from Friday evening to Sunday evening UTC; there are no candles then
  and Traidle simply waits.
- The chart picks up the number of decimals of the instrument automatically (5 for `EURUSD`, 3
  for `USDJPY`, 2 for gold and crypto).

**Security.** A Capital.com API key can trade, so Traidle is built to use it for market data only:
it calls just the login, server-time, market-lookup and price-history endpoints plus the quote
stream; it never calls any account, position, order or confirmation endpoint, never reads or
prints balances, and never prints or writes the key, the password or the session tokens. Keep
`.env` private (it is ignored by git and Docker builds); anyone holding it can trade your account.
Prefer the demo account while you are trying things out.

### Switching symbols without restarting

The **Disconnect** button at the top right of the chart (next to the connection status) ends the
current symbol's feed and takes you back to the market picker, so you can start a different
symbol - or the other market entirely - in the same running container. You never need to restart
`docker run` just to change what you are watching.

What happens under the hood: every worker thread for the old symbol is stopped (including an
in-progress historical backfill, which is cancelled promptly rather than being left to finish),
their CSV files are closed, and the chart is cleared before the picker reappears. The CSVs
already written are untouched - starting the same symbol again later resumes from them exactly
as a restart of the old, terminal-driven version would have. If a second browser tab is open, it
is kept in sync: it is shown the same picker/form/chart state as the tab that pressed Disconnect
or Start, and a start request from a tab that is not the active one is simply ignored while a
session is already running.

### Notes and limitations

- Binance: only USD-M **Futures** symbols are supported (e.g. `BTCUSDT`, `ETHUSDT`). Spot-only
  symbols or nonexistent tickers (e.g. `NVDAUSDT`) are not available.
- Capital.com: the epic must exist on your account's market list; a typo is reported before
  anything starts (`Unknown epic 'XXX' on Capital.com`).
- This is a market-data and charting tool only: there is no order placement or trading logic.
- TLS certificate verification is enabled (`ssl::verify_peer` with hostname verification). If
  the build environment has no system CA bundle configured for OpenSSL, set the `SSL_CERT_FILE`
  environment variable to a PEM bundle (e.g. Mozilla's `cacert.pem`) before running.