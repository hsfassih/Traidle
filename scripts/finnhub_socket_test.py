import json
import sys
import time
import requests
import websocket
from datetime import datetime, timezone

# Replace with your actual Finnhub API Key
API_KEY = "db39fjhr01qlusvlbtl0db39fjhr01qlusvlbtlg"

def get_market_symbols(market):
    market_config = {
        "Crypto": ("crypto/symbol", "binance"),
        "Stocks": ("stock/symbol", "US"),
        "Forex": ("forex/symbol", "oanda")
    }
    
    endpoint, exchange = market_config[market]
    url = f"https://finnhub.io/api/v1/{endpoint}?exchange={exchange}&token={API_KEY}"
    
    print(f"\nFetching {market} symbols from Finnhub...")
    response = requests.get(url)
    
    if response.status_code == 200:
        symbols = response.json()
        print(f"\n--- {market} Symbols ({exchange.upper()}) ---")
        for item in symbols[:50]: # Limiting to 50 for terminal readability
            symbol_id = item.get('symbol')
            desc = item.get('description', 'N/A')
            print(f"Symbol: {symbol_id:<20} | Description: {desc}")
        print(f"\nTotal {market} symbols found: {len(symbols)} (showing first 50)")
    else:
        print(f"Failed to fetch symbols. Status code: {response.status_code}")

def get_ohlc_data(market, symbol, timeframe):
    # Finnhub requires specific endpoints for each market's candles
    market_config = {
        "Crypto": "crypto/candle",
        "Stocks": "stock/candle",
        "Forex": "forex/candle"
    }
    endpoint = market_config[market]
    
    # Map your requested timeframes to Finnhub's URL resolution parameters and seconds
    resolution_map = {
        "1m": ("1", 60),
        "5m": ("5", 300),
        "15m": ("15", 900),
        "1h": ("60", 3600),
        "4h": ("240", 14400), # Note: Finnhub officially supports 1, 5, 15, 30, 60, D, W, M
        "1d": ("D", 86400),
        "1w": ("W", 604800),
        "1mo": ("M", 2592000)
    }
    
    if timeframe not in resolution_map:
        print(f"Invalid timeframe. Supported: {', '.join(resolution_map.keys())}")
        return
        
    res_val, seconds = resolution_map[timeframe]
    
    # Calculate Unix timestamps. We pull a 30-candle window back in time 
    # to guarantee we get data even if the market was closed for a weekend.
    to_ts = int(time.time())
    from_ts = to_ts - (seconds * 30)
    
    url = f"https://finnhub.io/api/v1/{endpoint}?symbol={symbol}&resolution={res_val}&from={from_ts}&to={to_ts}&token={API_KEY}"
    
    print(f"\nFetching {timeframe} OHLC data for {symbol}...")
    response = requests.get(url)
    
    if response.status_code == 200:
        data = response.json()
        if data.get('s') == 'ok':
            print(f"\n--- {symbol} | {timeframe} OHLC Data (Last 5 Candles) ---")
            print(f"{'Time (UTC)':<24} | {'Open':<10} | {'High':<10} | {'Low':<10} | {'Close':<10} | {'Volume'}")
            print("-" * 87)
            
            # Slice the last 5 items from the returned arrays
            limit = min(5, len(data['t']))
            for i in range(-limit, 0):
                # Convert the Unix timestamp (seconds) into a readable UTC datetime
                dt = datetime.fromtimestamp(data['t'][i], tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')
                o = data['o'][i]
                h = data['h'][i]
                l = data['l'][i]
                c = data['c'][i]
                # Volume might be 0 or missing for OTC Forex feeds
                v = data['v'][i] if 'v' in data else 0 
                
                print(f"{dt:<24} | {o:<10.5f} | {h:<10.5f} | {l:<10.5f} | {c:<10.5f} | {v}")
        elif data.get('s') == 'no_data':
            print("\nNo data found for this symbol/timeframe. (Note: Finnhub free tier may have delays or reject non-standard timeframes like '4h').")
        else:
             print("\nAPI Error:", data)
    else:
        print(f"\nFailed to fetch data. HTTP {response.status_code}: {response.text}")

def on_message(ws, message):
    data = json.loads(message)
    if data.get("type") == "trade":
        for trade in data.get("data", []):
            symbol = trade.get("s")
            price = trade.get("p")
            volume = trade.get("v")
            timestamp_ms = trade.get("t")
            
            if timestamp_ms:
                # Convert milliseconds (ws) to human-readable UTC format
                utc_time = datetime.fromtimestamp(timestamp_ms / 1000.0, tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')
            else:
                utc_time = "N/A"
                
            sys.stdout.write(f"\r[{utc_time}] Symbol: {symbol} | Price: {price} | Volume: {volume}          ")
            sys.stdout.flush()

def on_error(ws, error):
    print(f"\nError: {error}")

def on_close(ws, close_status_code, close_msg):
    print("\n\n### WebSocket Connection Closed ###")

def on_open(ws):
    print(f"Connected! Subscribing to live price action for {ws.user_symbol}...\n")
    ws.send(json.dumps({"type": "subscribe", "symbol": ws.user_symbol}))

def main():
    print("=== Finnhub Live Market Terminal ===")
    print("1. Crypto")
    print("2. Stocks")
    print("3. Forex")
    
    market_map = {"1": "Crypto", "2": "Stocks", "3": "Forex"}
    market_choice = input("\nSelect a market (1/2/3): ")
    
    if market_choice not in market_map:
        print("Invalid market selection.")
        return
        
    selected_market = market_map[market_choice]
    print(f"\n--- {selected_market} Market Selected ---")
    print(f"1. List all symbols for {selected_market}")
    print("2. Get live market price stream (Tick Data)")
    print("3. Get historical/current OHLC candlestick data (REST)")
    
    action_choice = input("\nSelect an option (1/2/3): ")
    
    if action_choice == "1":
        get_market_symbols(selected_market)
        
    elif action_choice == "2":
        print(f"\n(Note: Finnhub requires exchange prefixes. Crypto: BINANCE:BTCUSDT | Forex: OANDA:GBP_USD | Stocks: AAPL)")
        symbol = input("Enter the symbol to stream: ")
        
        ws_url = f"wss://ws.finnhub.io?token={API_KEY}"
        ws = websocket.WebSocketApp(ws_url, on_message=on_message, on_error=on_error, on_close=on_close)
        ws.user_symbol = symbol 
        ws.on_open = on_open
        ws.run_forever()
        
    elif action_choice == "3":
        print(f"\n(Note: Finnhub requires exchange prefixes. Crypto: BINANCE:BTCUSDT | Forex: OANDA:GBP_USD | Stocks: AAPL)")
        symbol = input("Enter the symbol to get OHLC data for: ")
        
        valid_timeframes = ["1m", "5m", "15m", "1h", "4h", "1d", "1w", "1mo"]
        print(f"Available timeframes: {', '.join(valid_timeframes)}")
        timeframe = input("Enter the timeframe: ")
        
        get_ohlc_data(selected_market, symbol, timeframe)
        
    else:
        print("Invalid action selection.")

if __name__ == "__main__":
    main()