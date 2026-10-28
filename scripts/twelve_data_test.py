import json
import sys
import requests
import websocket

# Replace with your actual Twelve Data API Key
API_KEY = "c3607eb3730b4677b5838a167933578a"

def get_market_symbols(market):
    # Twelve Data uses separate dedicated endpoints for each market
    market_config = {
        "Crypto": "cryptocurrencies",
        "Stocks": "stocks?country=United States", # Limiting to US stocks for a cleaner terminal output
        "Forex": "forex_pairs"
    }
    
    endpoint = market_config[market]
    # Handle the URL query parameter concatenation safely based on if there is already a '?'
    separator = "&" if "?" in endpoint else "?"
    url = f"https://api.twelvedata.com/{endpoint}{separator}apikey={API_KEY}"
    
    print(f"\nFetching {market} symbols from Twelve Data...")
    response = requests.get(url)
    
    if response.status_code == 200:
        data = response.json()
        
        # Twelve Data returns lists inside a 'data' array
        symbols = data.get("data", [])
        
        if not symbols:
            print("\nNo symbols found or API error:", data)
            return
            
        print(f"\n--- {market} Symbols ---")
        for item in symbols[:50]: # Limiting to 50 for terminal readability
            symbol_id = item.get('symbol', 'N/A')
            
            # Extract description dynamically depending on the market
            if market == "Forex":
                desc = f"{item.get('currency_base', '')} / {item.get('currency_quote', '')}"
            else:
                desc = item.get('currency_base', item.get('name', 'N/A'))
                
            print(f"Symbol: {symbol_id:<15} | Description: {desc}")
        print(f"\nTotal {market} symbols found: {len(symbols)} (showing first 50)")
    else:
        print(f"Failed to fetch symbols. Status code: {response.status_code}")

def get_ohlc_data(symbol, timeframe):
    # Map your requested user timeframes to Twelve Data's strict interval parameters
    resolution_map = {
        "1m": "1min",
        "5m": "5min",
        "15m": "15min",
        "1h": "1h",
        "4h": "4h",
        "1d": "1day",
        "1w": "1week",
        "1mo": "1month"
    }
    
    if timeframe not in resolution_map:
        print(f"Invalid timeframe. Supported: {', '.join(resolution_map.keys())}")
        return
        
    td_interval = resolution_map[timeframe]
    
    # Twelve Data's /time_series natively supports 'outputsize' to limit the returned candles
    url = f"https://api.twelvedata.com/time_series?symbol={symbol}&interval={td_interval}&outputsize=5&apikey={API_KEY}"
    
    print(f"\nFetching {timeframe} OHLC data for {symbol}...")
    response = requests.get(url)
    
    if response.status_code == 200:
        data = response.json()
        
        if 'values' in data:
            print(f"\n--- {symbol} | {timeframe} OHLC Data (Last 5 Candles) ---")
            print(f"{'Time':<20} | {'Open':<10} | {'High':<10} | {'Low':<10} | {'Close':<10} | {'Volume'}")
            print("-" * 80)
            
            # The 'values' array returns newest to oldest, so we reverse it to print chronologically
            candles = reversed(data['values'])
            for candle in candles:
                dt = candle.get('datetime', 'N/A')
                o = float(candle.get('open', 0))
                h = float(candle.get('high', 0))
                l = float(candle.get('low', 0))
                c = float(candle.get('close', 0))
                v = candle.get('volume', 0)
                
                print(f"{dt:<20} | {o:<10.5f} | {h:<10.5f} | {l:<10.5f} | {c:<10.5f} | {v}")
        elif data.get('status') == 'error':
            print(f"\nAPI Error: {data.get('message')}")
        else:
             print("\nUnexpected Response:", data)
    else:
        print(f"\nFailed to fetch data. HTTP {response.status_code}: {response.text}")

def on_message(ws, message):
    data = json.loads(message)
    
    # Twelve Data labels tick data with an "event" key set to "price"
    if data.get("event") == "price":
        symbol = data.get("symbol")
        price = data.get("price")
        # Twelve Data uses 'day_volume', which may return 0 or be missing for OTC Forex pairs
        volume = data.get("day_volume", 0) 
        
        sys.stdout.write(f"\r[Live Action] Symbol: {symbol} | Price: {price} | Day Volume: {volume}          ")
        sys.stdout.flush()
        
    elif data.get("event") == "subscribe-status":
        status = data.get("status")
        print(f"Subscription Status: {status}")
        if status != "ok":
             print(f"Message: {data}")
             
    elif data.get("status") == "error":
        print(f"\nWebSocket Error: {data.get('message')}")

def on_error(ws, error):
    print(f"\nError: {error}")

def on_close(ws, close_status_code, close_msg):
    print("\n\n### WebSocket Connection Closed ###")

def on_open(ws):
    print(f"Connected! Subscribing to live price action for {ws.user_symbol}...\n")
    # Twelve Data uses an action/params payload for subscriptions
    subscribe_msg = {
        "action": "subscribe",
        "params": {
            "symbols": ws.user_symbol
        }
    }
    ws.send(json.dumps(subscribe_msg))

def main():
    print("=== Twelve Data Live Market Terminal ===")
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
        print(f"\n(Note: Twelve Data does not use exchange prefixes. Just format it as AAPL, BTC/USD, or EUR/USD)")
        print(f"(Note: The free tier strictly limits WebSocket streaming to a predefined list of specific test symbols like AAPL or EUR/USD. Other assets will be rejected[cite: 1].)")
        symbol = input("Enter the symbol to stream: ")
        
        ws_url = f"wss://ws.twelvedata.com/v1/quotes/price?apikey={API_KEY}"
        ws = websocket.WebSocketApp(ws_url, on_message=on_message, on_error=on_error, on_close=on_close)
        ws.user_symbol = symbol 
        ws.on_open = on_open
        ws.run_forever()
        
    elif action_choice == "3":
        print(f"\n(Note: Twelve Data formats are standard tickers like AAPL, EUR/USD, BTC/USD)")
        symbol = input("Enter the symbol to get OHLC data for: ")
        
        valid_timeframes = ["1m", "5m", "15m", "1h", "4h", "1d", "1w", "1mo"]
        print(f"Available timeframes: {', '.join(valid_timeframes)}")
        timeframe = input("Enter the timeframe: ")
        
        get_ohlc_data(symbol, timeframe)
        
    else:
        print("Invalid action selection.")

if __name__ == "__main__":
    main()