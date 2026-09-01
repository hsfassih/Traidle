# Active Context

## Current State

The futures console application displays live OHLC candlestick data and builds successfully with Ninja and MSYS2 GCC.

## Latest Decision

Binance WebSocket subscriptions can complete a handshake without delivering frames in the tested environment. The application polls the verified USD-M Futures REST kline endpoint once per second instead, ensuring an immediate initial candle and ongoing updates.

The terminal renderer preserves closed candles as permanent lines and redraws only the current forming candle in place. It uses compact, width-aware output to prevent terminal wrapping from turning carriage-return updates into additional visible rows.

## Next Steps

- Add reconnection/backoff handling for production use.
- Consider configurable CA bundle discovery instead of the current fixed path.
- Expand to strategy integration only after market-data behavior is stable.
