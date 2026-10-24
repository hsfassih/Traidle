#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "pipeline_common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using namespace std;

namespace pipeline {

string trim(const string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

string toUpper(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(toupper(c)); });
    return value;
}

string toLower(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return value;
}

// ---------------------------------------------------------------------------
// TerminalBoard (moved verbatim from binance.cpp; see pipeline_common.h for
// the class comment. Only change: member functions are defined out of line.)
// ---------------------------------------------------------------------------
TerminalBoard::TerminalBoard(vector<string> labels)
    : labels_(move(labels)), ansiEnabled_(enableAnsi()) {
    lock_guard<mutex> lock(mutex_);
    for (const auto& label : labels_) {
        cout << "[" << label << "] waiting for data...\n";
    }
    cout << flush;
}

void TerminalBoard::updateLine(size_t index, const string& text) {
    lock_guard<mutex> lock(mutex_);
    const string full = "[" + labels_[index] + "] " + text;
    if (!ansiEnabled_) {
        cout << full << '\n' << flush;
        return;
    }
    const size_t upCount = (labels_.size() - index) + extraLines_;
    cout << "\x1b[" << upCount << "A\r\x1b[2K" << full
         << "\x1b[" << upCount << "B\r" << flush;
}

void TerminalBoard::log(const string& text) {
    lock_guard<mutex> lock(mutex_);
    cerr << text << '\n' << flush;
    if (ansiEnabled_) {
        ++extraLines_;
    }
}

bool TerminalBoard::enableAnsi() {
#if defined(_WIN32)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode)) {
        return false;
    }
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
           SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

size_t terminalColumns() {
#if defined(_WIN32)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO bufferInfo{};
    if (output != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(output, &bufferInfo)) {
        return static_cast<size_t>(bufferInfo.srWindow.Right - bufferInfo.srWindow.Left + 1);
    }
#endif
    return 0;
}

string formatPrice(double price) {
    ostringstream oss;
    oss << setprecision(10) << defaultfloat << price;
    return oss.str();
}

string formatUtcTimestamp(int64_t epochMs) {
    const time_t epochSeconds = static_cast<time_t>(epochMs / 1000);
    tm utcTm{};
#if defined(_WIN32)
    gmtime_s(&utcTm, &epochSeconds);
#else
    gmtime_r(&epochSeconds, &utcTm);
#endif
    ostringstream oss;
    oss << put_time(&utcTm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

string formatCandleLine(const string& symbol, const candlesticks::Candlestick& candle,
                        const char* stateTag, bool compact) {
    const string open = formatPrice(candle.open);
    const string high = formatPrice(candle.high);
    const string low = formatPrice(candle.low);
    const string close = formatPrice(candle.close);
    const auto trend = candlesticks::trendOf(candle);
    if (!compact) {
        return symbol + " O:" + open + " H:" + high + " L:" + low + " C:" + close + " " +
               candlesticks::trendLabel(trend) + " " + stateTag;
    }
    const char* code = trend == candlesticks::Trend::Bullish ? "B" :
                        trend == candlesticks::Trend::Bearish ? "S" : "N";
    return "O:" + open + " H:" + high + " L:" + low + " C:" + close + " " + code + " " +
           stateTag;
}

// ---------------------------------------------------------------------------
// Timeframe -> English CSV filename mapping
// ---------------------------------------------------------------------------
optional<string> englishNameForInterval(const string& binanceInterval) {
    static const unordered_map<string, string> kNames = {
        {"1m", "one_minute"},   {"3m", "three_minutes"}, {"5m", "five_minutes"},
        {"15m", "fifteen_minutes"}, {"30m", "thirty_minutes"}, {"1h", "one_hour"},
        {"2h", "two_hours"},    {"4h", "four_hours"},    {"6h", "six_hours"},
        {"8h", "eight_hours"},  {"12h", "twelve_hours"}, {"1d", "one_day"},
        {"3d", "three_days"},   {"1w", "one_week"},      {"1M", "one_month"},
    };
    const auto it = kNames.find(binanceInterval);
    if (it == kNames.end()) {
        return nullopt;
    }
    return it->second;
}

vector<string> splitByComma(const string& raw) {
    vector<string> parts;
    stringstream ss(raw);
    string item;
    while (getline(ss, item, ',')) {
        parts.push_back(item);
    }
    return parts;
}

ofstream openCsvAppend(const filesystem::path& path) {
    const bool needsHeader = !filesystem::exists(path) || filesystem::file_size(path) == 0;
    ofstream csv(path, ios::app);
    if (!csv.is_open()) {
        throw runtime_error("Failed to open CSV file: " + path.string());
    }
    if (needsHeader) {
        csv << "timestamp,base_volume,quote_volume,taker_buy_base_volume,taker_buy_quote_volume,"
            << indicators::indicatorHeader() << "open,high,low,close\n";
        csv.flush();
    }
    return csv;
}

// Builds the flat wire-format message the visualization layer broadcasts,
// out of whatever candle + indicator snapshot we've already computed for
// the CSV row. Shared by the live-write path (writeClosedCandleRow below),
// the live-forming-candle path (renderLine, inside runTimeframeStream),
// and the backfill/replay seed-history hook (also inside runTimeframeStream),
// so all three never drift out of sync on field mapping.
visualization::VisualizationMessage buildVisualizationMessage(
    const string& symbol, const string& timeframeLabel, const candlesticks::Candlestick& candle,
    const indicators::IndicatorSnapshot& snapshot) {
    visualization::VisualizationMessage message;
    message.symbol = symbol;
    message.timeframe = timeframeLabel;
    message.openTime = candle.openTime;
    message.closeTime = candle.closeTime;
    message.closed = candle.closed;
    message.open = candle.open;
    message.high = candle.high;
    message.low = candle.low;
    message.close = candle.close;
    message.baseVolume = candle.baseVolume;
    message.quoteVolume = candle.quoteVolume;
    message.takerBuyBaseVolume = candle.takerBuyBaseVolume;
    message.takerBuyQuoteVolume = candle.takerBuyQuoteVolume;
    message.indicatorValues = snapshot;
    return message;
}

// engine.update(candle) + CSV row + flush, without any visualizer push.
// This is the first half of the original writeClosedCandleRow(), unchanged
// line for line; the Capital.com backfill uses it directly because backfilled
// rows go to the visualizer through seedHistory(), not through push().
indicators::IndicatorSnapshot writeCsvRow(ofstream& csv, const candlesticks::Candlestick& candle,
                                          indicators::IndicatorEngine& engine) {
    const auto snapshot = engine.update(candle);
    csv << formatUtcTimestamp(candle.openTime) << ','
        << formatPrice(candle.baseVolume) << ','
        << formatPrice(candle.quoteVolume) << ','
        << formatPrice(candle.takerBuyBaseVolume) << ','
        << formatPrice(candle.takerBuyQuoteVolume) << ','
        << indicators::formatIndicatorRow(snapshot)
        << formatPrice(candle.open) << ','
        << formatPrice(candle.high) << ','
        << formatPrice(candle.low) << ','
        << formatPrice(candle.close) << '\n';
    csv.flush();
    return snapshot;
}

// Writes one closed candle's CSV row (indicators + OHLCV) exactly as
// before, and additionally returns the IndicatorSnapshot that
// engine.update(candle) just produced - needed by the caller to feed
// smc-ict/liquidity.cpp's ATR-scaled Equal-Highs/Lows tolerance (see
// smc_engine.h). IndicatorEngine::update() must be called exactly once per
// closed candle (its own documented contract), so this snapshot has to be
// captured and handed back here rather than the SMC path calling
// engine.update() a second time itself.
indicators::IndicatorSnapshot writeClosedCandleRow(ofstream& csv, const candlesticks::Candlestick& candle,
                          indicators::IndicatorEngine& engine,
                          visualization::VisualizationServer& vizServer, const string& symbol,
                          const string& timeframeLabel) {
    const auto snapshot = writeCsvRow(csv, candle, engine);

    // Tap point for the visualization layer: same snapshot just computed
    // above, handed off as a plain data copy. push() is thread-safe,
    // non-blocking, and never throws, so it can never delay the CSV write
    // above it or anything else in this function's caller.
    vizServer.push(buildVisualizationMessage(symbol, timeframeLabel, candle, snapshot));
    return snapshot;
}

// ---------------------------------------------------------------------------
// .env loader
// ---------------------------------------------------------------------------
size_t loadDotEnv(const filesystem::path& path) {
    ifstream file(path);
    if (!file.is_open()) {
        return 0;
    }

    size_t applied = 0;
    bool firstLine = true;
    string line;
    while (getline(file, line)) {
        // A UTF-8 byte-order mark (Windows Notepad adds one) would otherwise
        // glue itself onto the first variable's name and silently break it.
        if (firstLine && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        firstLine = false;

        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("export ", 0) == 0) {
            line = trim(line.substr(7));
        }
        const auto equals = line.find('=');
        if (equals == string::npos || equals == 0) {
            continue;
        }
        const string key = trim(line.substr(0, equals));
        string value = trim(line.substr(equals + 1));
        if (key.empty()) {
            continue;
        }

        const bool quoted = value.size() >= 2 &&
                            ((value.front() == '"' && value.back() == '"') ||
                             (value.front() == '\'' && value.back() == '\''));
        if (quoted) {
            value = value.substr(1, value.size() - 2);
        } else {
            // Unquoted values may carry a trailing " # comment". A '#' with no
            // space before it is left alone (it can legitimately be part of a
            // password).
            const auto hash = value.find(" #");
            if (hash != string::npos) {
                value = trim(value.substr(0, hash));
            }
        }
        if (value.empty()) {
            continue;  // KEY= with nothing after it means "unset"; don't create an empty variable.
        }

        // The real environment always wins (docker --env-file, shell export).
        if (getenv(key.c_str()) != nullptr) {
            continue;
        }
#if defined(_WIN32)
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 0);
#endif
        ++applied;
    }
    return applied;
}

// ---------------------------------------------------------------------------
// Timeframe validation, shared by both markets. The loop and every message in
// it are the ones main() in binance.cpp used to contain.
// ---------------------------------------------------------------------------
vector<TimeframeTask> buildTasks(const string& rawTimeframes, vector<string>* skipped) {
    vector<TimeframeTask> tasks;
    set<string> seenIntervals;
    const auto skip = [skipped](const string& message) {
        if (skipped != nullptr) {
            skipped->push_back(message);
        } else {
            cerr << message << endl;
        }
    };
    for (const auto& piece : splitByComma(rawTimeframes)) {
        const string timeframe = trim(piece);
        if (timeframe.empty()) {
            continue;
        }
        const auto binanceInterval = candlesticks::toBinanceInterval(timeframe);
        if (!binanceInterval.has_value()) {
            skip("Skipping unsupported timeframe '" + timeframe + "'. Choose from: " +
                 candlesticks::supportedTimeframes());
            continue;
        }
        if (!seenIntervals.insert(*binanceInterval).second) {
            skip("Skipping duplicate timeframe '" + timeframe + "'.");
            continue;
        }
        const auto englishName = englishNameForInterval(*binanceInterval);
        if (!englishName.has_value()) {
            skip("No CSV filename mapping for interval '" + *binanceInterval + "'; skipping.");
            continue;
        }

        TimeframeTask task;
        task.rawTimeframe = timeframe;
        task.binanceInterval = *binanceInterval;
        task.englishName = *englishName;
        task.rowIndex = tasks.size();
        tasks.push_back(move(task));
    }
    return tasks;
}

}  // namespace pipeline