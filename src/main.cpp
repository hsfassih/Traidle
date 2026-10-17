#include "binance.h"
#include "candlesticks.h"
#include "capital.h"
#include "pipeline_common.h"
#include "visualization.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace std;

namespace {

// Port the local (loopback-only) visualization WebSocket server listens
// on. Change here and in web/visualizer.html's port field if it conflicts
// with something else on your machine.
constexpr unsigned short kVisualizationPort = 8765;

// Capital.com data lives in fx_<epic>/ (fx_eurusd, fx_xauusd, ...), never next
// to the Binance symbol folders: a Binance symbol such as "eurusdt" could
// otherwise collide with a Forex epic of the same lower-cased name.
constexpr const char* kCapitalFolderPrefix = "fx_";

enum class Market { Binance, Capital };

// argv[1] ("binance" / "capital") skips the prompt, which makes the app
// scriptable (docker run ... traidle capital). Otherwise ask; an empty answer
// means Binance so the old "just press Enter" muscle memory keeps working.
bool chooseMarket(int argc, char** argv, Market& market) {
    string choice;
    if (argc > 1) {
        choice = pipeline::toLower(pipeline::trim(argv[1]));
    } else {
        cout << "Select market: [1] Binance USD-M Futures  [2] Forex/CFD via Capital.com : ";
        if (!getline(cin, choice)) {
            cerr << "No input provided." << endl;
            return false;
        }
        choice = pipeline::toLower(pipeline::trim(choice));
    }

    if (choice.empty() || choice == "1" || choice == "binance") {
        market = Market::Binance;
        return true;
    }
    if (choice == "2" || choice == "capital") {
        market = Market::Capital;
        return true;
    }
    cerr << "Unknown market '" << choice << "'. Choose 1 (Binance) or 2 (Capital.com)." << endl;
    return false;
}

// The original Binance flow from the old main(), unchanged: symbol ->
// timeframes -> folder lower(symbol) -> board -> visualizer -> one thread per
// timeframe.
int runBinance() {
    cout << "Enter a Binance USD-M Futures symbol (e.g. BTCUSDT): ";
    string rawSymbol;
    if (!getline(cin, rawSymbol)) {
        cerr << "No input provided." << endl;
        return 1;
    }
    const string symbol = pipeline::toUpper(pipeline::trim(rawSymbol));
    if (symbol.empty()) {
        cerr << "Symbol must not be empty." << endl;
        return 1;
    }

    cout << "Enter candlestick timeframes, comma-separated (e.g. 1m,15m,1h,1d): ";
    string rawTimeframes;
    if (!getline(cin, rawTimeframes)) {
        cerr << "No timeframe input provided." << endl;
        return 1;
    }

    vector<pipeline::TimeframeTask> tasks = pipeline::buildTasks(rawTimeframes);
    if (tasks.empty()) {
        cerr << "No valid timeframes provided. Exiting." << endl;
        return 1;
    }

    const string symbolFolder = pipeline::toLower(symbol);
    error_code dirError;
    filesystem::create_directories(symbolFolder, dirError);
    if (dirError) {
        cerr << "Failed to create folder '" << symbolFolder << "': " << dirError.message()
             << endl;
        return 1;
    }

    cout << "Streaming " << symbol << " across " << tasks.size()
         << " timeframe(s). Closed candles are appended to CSVs under '" << symbolFolder
         << "/'. Press Ctrl+C to exit.\n";

    vector<string> labels;
    labels.reserve(tasks.size());
    for (const auto& task : tasks) {
        labels.push_back(task.rawTimeframe);
    }
    pipeline::TerminalBoard board(labels);

    // Started once, shared by every timeframe worker below (single-viewer/
    // local by design - see visualization.h). A bind failure here only
    // disables live charting; it never stops candle streaming, CSV
    // writes, or indicator computation.
    visualization::VisualizationServer vizServer(kVisualizationPort);
    vizServer.start();

    vector<thread> workers;
    workers.reserve(tasks.size());
    for (const auto& task : tasks) {
        filesystem::path csvPath = filesystem::path(symbolFolder) / (task.englishName + ".csv");
        workers.emplace_back(binance::runTimeframeStream, symbol, task, move(csvPath),
                             ref(board), ref(vizServer));
    }

    for (auto& worker : workers) {
        worker.join();
    }
    return 0;
}

// Forex/CFD via Capital.com: epic -> timeframes -> credentials -> login and
// epic check -> folder fx_<epic> -> board -> visualizer -> feed + one worker per
// timeframe. The pre-flight runs BEFORE the TerminalBoard exists so a missing
// credential or a mistyped epic prints as plain text instead of fighting the
// board's cursor control.
int runCapital() {
    cout << "Enter a Capital.com epic (e.g. EURUSD, GBPUSD, USDJPY, XAUUSD): ";
    string rawEpic;
    if (!getline(cin, rawEpic)) {
        cerr << "No input provided." << endl;
        return 1;
    }
    const string epic = pipeline::toUpper(pipeline::trim(rawEpic));
    if (epic.empty()) {
        cerr << "Epic must not be empty." << endl;
        return 1;
    }

    cout << "Enter candlestick timeframes, comma-separated (e.g. 1m,15m,1h,1d): ";
    string rawTimeframes;
    if (!getline(cin, rawTimeframes)) {
        cerr << "No timeframe input provided." << endl;
        return 1;
    }

    vector<pipeline::TimeframeTask> tasks = pipeline::buildTasks(rawTimeframes);
    if (tasks.empty()) {
        cerr << "No valid timeframes provided. Exiting." << endl;
        return 1;
    }

    // Credentials come from the environment (docker --env-file, or a local .env
    // next to the binary). loadDotEnv never overrides a variable that is already
    // set, so the real environment always wins over the file.
    pipeline::loadDotEnv("./.env");

    string errorText;
    shared_ptr<capital::Session> session = capital::preflight(epic, errorText);
    if (!session) {
        cerr << errorText << endl;
        return 1;
    }

    const string folder = string(kCapitalFolderPrefix) + pipeline::toLower(epic);
    error_code dirError;
    filesystem::create_directories(folder, dirError);
    if (dirError) {
        cerr << "Failed to create folder '" << folder << "': " << dirError.message() << endl;
        return 1;
    }

    cout << "Streaming " << epic << " across " << tasks.size()
         << " timeframe(s). Closed candles are appended to CSVs under '" << folder
         << "/'. Press Ctrl+C to exit.\n";

    vector<string> labels;
    labels.reserve(tasks.size());
    for (const auto& task : tasks) {
        labels.push_back(task.rawTimeframe);
    }
    pipeline::TerminalBoard board(labels);

    visualization::VisualizationServer vizServer(kVisualizationPort);
    vizServer.start();

    capital::run(session, epic, tasks, filesystem::path(folder), board, vizServer);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Market market = Market::Binance;
    if (!chooseMarket(argc, argv, market)) {
        return 1;
    }

    switch (market) {
        case Market::Binance:
            return runBinance();
        case Market::Capital:
            return runCapital();
    }
    return 1;
}