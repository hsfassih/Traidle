#include "binance/binance.h"
#include "capital/capital.h"
#include "utils/candlesticks.h"
#include "utils/pipeline_common.h"
#include "session_manager.h"
#include "visualization.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std;

namespace {

// Port the visualization WebSocket server listens on. Change here and in
// web/visualizer.html's port field if it conflicts with something else on your
// machine.
constexpr unsigned short kVisualizationPort = 8765;

// The real market adapters, handed to the SessionManager. (The manager itself
// does not link against binance.cpp, so tests can substitute fakes.)
app::Backends productionBackends() {
    app::Backends backends;

    // ---- Binance USD-M Futures -------------------------------------------
    backends.checkBinanceSymbol = binance::checkSymbol;

    // The original Binance flow: one worker thread per timeframe, each writing its
    // own CSV under the symbol's folder. Returns once every worker has ended,
    // which happens when `stop` fires.
    backends.runBinance = [](const string& symbol, const vector<pipeline::TimeframeTask>& tasks,
                             const filesystem::path& folder, pipeline::TerminalBoard& board,
                             visualization::VisualizationServer& viz, app::StopSignal& stop) {
        vector<thread> workers;
        workers.reserve(tasks.size());
        for (const auto& task : tasks) {
            filesystem::path csvPath = folder / (task.englishName + ".csv");
            workers.emplace_back(binance::runTimeframeStream, symbol, task, move(csvPath),
                                 ref(board), ref(viz), &stop);
        }
        for (auto& worker : workers) {
            worker.join();
        }
    };

    // ---- Forex/CFD via Capital.com ---------------------------------------
    backends.capitalPreflight = [](const string& epic, string& errorText) {
        // Credentials come from the environment (docker --env-file, or a local .env
        // next to the binary). loadDotEnv never overrides a variable that is already
        // set, so the real environment always wins over the file. (It is read when a
        // Capital.com session starts; edit .env and restart the app to change it.)
        pipeline::loadDotEnv("./.env");
        return capital::preflight(epic, errorText);
    };
    backends.runCapital = [](shared_ptr<capital::Session> session, const string& epic,
                             const vector<pipeline::TimeframeTask>& tasks,
                             const filesystem::path& folder, pipeline::TerminalBoard& board,
                             visualization::VisualizationServer& viz, app::StopSignal& stop) {
        capital::RunOptions options;
        options.stop = &stop;
        capital::run(move(session), epic, tasks, folder, board, viz, options);
    };

    return backends;
}

}  // namespace

// The market, symbol and timeframes are no longer asked for on the terminal:
// the browser page (web/visualizer.html) offers the market picker, takes the
// symbol and timeframes, and has the Disconnect button that returns to the
// picker - all without restarting this process. See session_manager.h.
int main(int argc, char** argv) {
    (void)argv;
    if (argc > 1) {
        cerr << "Note: command-line arguments are no longer used; choose the market in the browser.\n";
    }

    // Started once for the whole process and shared by every session (single-viewer/
    // local by design - see visualization.h). The manager registers itself with the
    // server, so it is created first.
    visualization::VisualizationServer vizServer(kVisualizationPort);
    app::SessionManager manager(vizServer, productionBackends());
    vizServer.start();
    if (!vizServer.isRunning()) {
        // The browser is the only way to drive the app now, so unlike before there is
        // nothing useful to do without the server. (start() already printed the reason.)
        cerr << "Cannot continue without the browser interface on port " << kVisualizationPort
             << ". Free that port, or change kVisualizationPort in src/main.cpp (and the port "
                "field in web/visualizer.html).\n";
        return 1;
    }
    cout << "Open web/visualizer.html in your browser to choose a market, symbol and timeframes. "
            "Press Ctrl+C to exit.\n";

    for (;;) {
        this_thread::sleep_for(chrono::hours(24));  // everything happens on the manager's and server's threads
    }
}