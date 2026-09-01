# Project Brief

## Traidle

A C++ console application for monitoring live Binance USD-M Futures market prices. The user enters one trading symbol and sees the latest valid trade price on the console every second.

## Initial Scope

- One user-selected USD-M Futures symbol per process.
- Public live market data only; no API credentials or trading actions.
- TLS-secured Binance WebSocket connection.
- CMake and vcpkg-based Windows build.
