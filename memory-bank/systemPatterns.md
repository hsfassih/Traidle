# System Patterns

- The executable uses `src/main.cpp` and the `src/candlesticks.cpp` parsing module.
- Boost.Asio and Boost.Beast provide TLS HTTPS networking.
- OpenSSL verifies the Binance TLS certificate with an explicit PEM CA bundle.
- `nlohmann::json` parses Binance REST kline responses.
- The application polls `https://fapi.binance.com/fapi/v1/klines` with the selected symbol and interval once per second.
- The candlestick module validates supported Binance intervals, accepting `mo` for monthly input and converting it to the API's `M` code.
- Each valid response supplies finite positive open, high, low, and close prices, plus a bullish, bearish, or neutral trend.
- `CandlestickDisplay` is the only candle-output owner: forming updates use carriage return and no newline, while a candle rollover commits the previous candle once with a newline.
- Live records use a compact, terminal-width-aware layout so physical line wrapping cannot break in-place updates.
