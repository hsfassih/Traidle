#pragma once

#include "candlesticks.h"
#include "indicators.h"
#include "visualization.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// pipeline: the console / CSV / visualizer glue that every market feed
// (Binance today, Capital.com next to it) shares. Everything here except the
// three "new" helpers at the bottom (writeCsvRow, loadDotEnv, buildTasks) was
// moved verbatim out of binance.cpp, where it used to live in an anonymous
// namespace next to main(). It is deliberately feed-agnostic: nothing in this
// file knows about Binance's or Capital.com's wire format, so a new market
// adapter only has to produce `candlesticks::Candlestick`s and call into it.
// ---------------------------------------------------------------------------
namespace pipeline {

string trim(const string& value);
string toUpper(string value);
string toLower(string value);

// ---------------------------------------------------------------------------
// TerminalBoard: replaces CandlestickDisplay. The old class committed each
// closed candle as a permanent scrollback line; that model no longer applies
// now that closed candles live in CSV files instead. This board instead
// reserves one fixed, in-place-updating line per timeframe (top to bottom,
// in the order the user typed them) using ANSI cursor movement, guarded by a
// single mutex so multiple stream threads can never interleave writes.
//
// `log()` is the only other thing allowed to touch stdout/stderr after
// construction; it tracks how many extra lines have scrolled below the
// board so the cursor math for updateLine() stays correct indefinitely.
// candlesticks.h / candlesticks.cpp are not touched by any of this.
//
// (Declared here, defined in pipeline_common.cpp, so that <windows.h> - which
// enableAnsi() needs - never leaks into every translation unit that merely
// wants to log a line.)
// ---------------------------------------------------------------------------
class TerminalBoard {
public:
    explicit TerminalBoard(vector<string> labels);

    void updateLine(size_t index, const string& text);
    void log(const string& text);

private:
    static bool enableAnsi();

    vector<string> labels_;
    bool ansiEnabled_;
    size_t extraLines_ = 0;
    mutex mutex_;
};

size_t terminalColumns();
string formatPrice(double price);
string formatUtcTimestamp(int64_t epochMs);
string formatCandleLine(const string& symbol, const candlesticks::Candlestick& candle,
                        const char* stateTag, bool compact);

// Timeframe -> English CSV filename mapping ("1m" -> "one_minute", ...).
optional<string> englishNameForInterval(const string& binanceInterval);
vector<string> splitByComma(const string& raw);

struct TimeframeTask {
    string rawTimeframe;
    string binanceInterval;
    string englishName;
    size_t rowIndex = 0;
};

ofstream openCsvAppend(const filesystem::path& path);

// Builds the flat wire-format message the visualization layer broadcasts,
// out of whatever candle + indicator snapshot we've already computed for
// the CSV row. Shared by the live-write path (writeClosedCandleRow), the
// live-forming-candle path, and the backfill/replay seed-history hook, so
// all three never drift out of sync on field mapping.
visualization::VisualizationMessage buildVisualizationMessage(
    const string& symbol, const string& timeframeLabel, const candlesticks::Candlestick& candle,
    const indicators::IndicatorSnapshot& snapshot);

// Writes one closed candle's CSV row and pushes it to the visualizer; returns
// the IndicatorSnapshot that engine.update(candle) just produced (needed by
// the caller to feed the SMC engine's ATR-scaled tolerance). Behavior is
// identical to the original binance.cpp helper: it is now writeCsvRow()
// followed by the visualizer push.
indicators::IndicatorSnapshot writeClosedCandleRow(ofstream& csv,
                                                   const candlesticks::Candlestick& candle,
                                                   indicators::IndicatorEngine& engine,
                                                   visualization::VisualizationServer& vizServer,
                                                   const string& symbol,
                                                   const string& timeframeLabel);

// ---------------------------------------------------------------------------
// New helpers (not moved from binance.cpp)
// ---------------------------------------------------------------------------

// engine.update(candle) + the 23-column CSV row + flush, and NO visualizer
// push. Used by the Capital.com history backfill, which hands the (candle,
// snapshot) pair to the visualizer's seedHistory() instead of broadcasting
// it live. writeClosedCandleRow() is this plus the live push.
indicators::IndicatorSnapshot writeCsvRow(ofstream& csv, const candlesticks::Candlestick& candle,
                                          indicators::IndicatorEngine& engine);

// Tiny .env loader: KEY=VALUE lines, optional "export " prefix, single or
// double quotes around the value, '#' comment lines and trailing " #..."
// comments on unquoted values. NEVER overrides a variable that is already set
// in the real environment (so `docker run --env-file` / a shell export always
// wins over a stale file). Returns how many variables it actually set. Does
// not print anything - values are credentials.
size_t loadDotEnv(const filesystem::path& path);

// The timeframe-validation loop that used to live in Binance's main(): same
// messages, same skipping rules, shared by both markets. Returns an empty
// vector when nothing valid was given (the caller prints the final error).
vector<TimeframeTask> buildTasks(const string& rawTimeframes);

}  // namespace pipeline