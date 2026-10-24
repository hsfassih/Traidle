# Progress

## Working

- CMake/vcpkg project configuration.
- User symbol normalization.
- User-selected validated candlestick timeframe.
- One-second TLS REST polling of Binance USD-M Futures klines.
- A single in-place Open/High/Low/Close row for the forming candle.
- One permanent final Open/High/Low/Close row for each closed candle.
- Docker image publication to GitHub Container Registry for runtime use.
- Web visualizer: visible Kill Zone bands + top-strip markers, Sessions toggle, responsive layout (700-1920px).
- Capital.com weekly history backfill ignores misaligned overfetched bars before its requested lower bound while preserving UTC-alignment checks for bars in range.
- Capital.com historical paging accepts the exact `error.prices.not-found` 404 only on older-page requests as an end-of-history signal; other failures remain errors.
- Shared CSV history scanning/replay extracted to `src/utils/csv_history`; Capital no longer compiles or includes Binance history code. CMake targets point at the reorganized source tree.

## Known Constraints

- The application uses REST polling because WebSocket frames were not delivered in the tested environment.
- TLS requires `C:/certs/cacert.pem`.
- The current version has no automatic reconnect logic.
