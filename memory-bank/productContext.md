# Product Context

Traidle provides a lightweight starting point for a future trading application by exposing live futures price action in a console.

Users enter a Binance-listed USD-M Futures symbol such as `BTCUSDT`. The program normalizes the input, connects to Binance, and prints the latest known last-traded price once per second. A previous valid price remains visible during periods without a new trade.
