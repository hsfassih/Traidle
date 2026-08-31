# TRAIDLE
AI powered trading - An integration of live market stream with your custom trading strategy

## Live Futures Price Console

A C++17 console application that connects to Binance's USD-M Futures WebSocket stream
(`wss://fstream.binance.com/ws/<symbol>@trade`) and prints the latest trade price for one symbol,
updated roughly once per second.

### Prerequisites

- CMake 3.16+
- A C++17 compiler (MSVC via Visual Studio Build Tools, or another supported Windows toolchain)
- [vcpkg](https://github.com/microsoft/vcpkg), used to fetch Boost (Asio/Beast), OpenSSL, and
  nlohmann-json declared in `vcpkg.json`

### Build

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

### Run

```powershell
./build/Release/traidle.exe
```

You will be prompted for a symbol, e.g.:

```
Enter a Binance USD-M Futures symbol (e.g. BTCUSDT): BTCUSDT
Streaming BTCUSDT last trade price. Press Ctrl+C to exit.
BTCUSDT  63241.50000000  [14:32:11]
```

The line refreshes in place as new mark-price updates arrive from Binance.

### Notes and limitations

- Only Binance USD-M **Futures** symbols are supported (e.g. `BTCUSDT`, `ETHUSDT`). Spot-only
  symbols or nonexistent tickers (e.g. `NVDAUSDT`, since NVIDIA is not a Binance Futures
  contract) are not available on this stream.
- The displayed value is the latest **last-traded price** from Binance's `@trade` stream, not
  the futures mark price. The display is limited to one update per second.
- This is a public market-data client only: no API key, order placement, or trading logic is
  included.
- TLS certificate verification is enabled (`ssl::verify_peer` with hostname verification). If
  the build environment has no system CA bundle configured for OpenSSL, set the `SSL_CERT_FILE`
  environment variable to a PEM bundle (e.g. Mozilla's `cacert.pem`) before running.
