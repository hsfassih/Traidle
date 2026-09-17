# Progress

## Working

- CMake/vcpkg project configuration.
- User symbol normalization.
- User-selected validated candlestick timeframe.
- One-second TLS REST polling of Binance USD-M Futures klines.
- A single in-place Open/High/Low/Close row for the forming candle.
- One permanent final Open/High/Low/Close row for each closed candle.
- Docker image publication to GitHub Container Registry for runtime use.

## Known Constraints

- The application uses REST polling because WebSocket frames were not delivered in the tested environment.
- TLS requires `C:/certs/cacert.pem`.
- The current version has no automatic reconnect logic.
