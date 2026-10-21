# Active Context

## Current State

The futures console application displays live OHLC candlestick data and is distributed as a Docker image from GitHub Container Registry. The runtime is intended to be launched from a dedicated working folder so the container can keep its data volume isolated and consistent.

## Latest Decision

Binance WebSocket subscriptions can complete a handshake without delivering frames in the tested environment. The application polls the verified USD-M Futures REST kline endpoint once per second instead, ensuring an immediate initial candle and ongoing updates.

Capital.com weekly history can over-return old bars with non-UTC week timestamps. Weekly backfill now trims bars older than the requested lower bound before alignment validation; shifted weekly bars inside the requested range are still rejected. The live AUDCAD smoke test wrote 261 aligned candles.

Capital.com 15-minute pagination now treats only an older-page HTTP 404 with `error.prices.not-found` as end-of-history, preserving and writing collected candles. Initial-page 404s and all other errors still fail. The EURAUD live smoke test wrote 69,812 ordered, aligned candles through the provider's available-history boundary.

The terminal renderer preserves closed candles as permanent lines and redraws only the current forming candle in place. It uses compact, width-aware output to prevent terminal wrapping from turning carriage-return updates into additional visible rows.

## Recent Frontend Changes (web/visualizer.html, 2026-09-26)

- Kill Zone bands were invisible: LWC pane canvases are z-index 1 (opaque) / 2 and the overlay sat at z-index 0. All overlays now use z-index >= 3.
- Shared top-strip marker renderer (`renderTimeWindowMarkers()`, `#sessionMarkersOverlay`) draws start/end markers for Kill Zones (lane 0) and new Sessions (SYD/TOK/LON/NY, fixed 9h NY-time windows, wraparound-aware).
- Wall-clock times are mapped via `timeToX()` (manual interpolation between bar coordinates).
- Responsive: readout moves below the chart at <=900px; `ResizeObserver` on `#chartContainer` drives `resizeChart()`.

## Next Steps

- Add reconnection/backoff handling for production use.
- Consider configurable CA bundle discovery instead of the current fixed path.
- Expand to strategy integration only after market-data behavior is stable.
