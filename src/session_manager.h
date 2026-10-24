#pragma once

#include "capital.h"
#include "pipeline_common.h"
#include "stop_signal.h"
#include "visualization.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// app::SessionManager: lets the BROWSER choose the market, symbol and
// timeframes, and switch to another symbol later, without restarting the
// process. It replaces the old stdin prompts in main().
//
//   idle --start--> starting --> running --disconnect--> stopping --> idle
//                       \_(rejected: bad input / unknown symbol / login failed)_/
//
// "A session" is exactly what one run of the old program was: one symbol, one
// worker thread per timeframe (plus the shared Capital.com quote feed), one
// TerminalBoard, one set of CSV files. The manager starts one when the browser
// sends a `start` command and tears it down - stops every worker, joins them,
// wipes the visualizer's buffered chart data - on `disconnect`, after which a
// new one can start. The workers themselves are unchanged apart from being
// stoppable (stop_signal.h).
//
// Wire protocol (JSON text frames over the existing visualization WebSocket):
//   browser -> app   {"type":"start","market":"binance"|"capital",
//                     "symbol":"BTCUSDT","timeframes":"1m,15m,1h"}
//                    {"type":"disconnect"}
//   app -> browser   {"type":"session_state","state":"idle"|"starting"|"running"|"stopping",
//                     "market":..., "symbol":..., "timeframes":[...],   // when known
//                     "error":"...",       // only on a state=idle that follows a failure
//                     "warnings":[...]}    // e.g. a skipped, unsupported timeframe
// The current state is also what every newly connected page receives first, so
// a refreshed or second tab lands on the right screen.
//
// Threading: handleMessage() is called on the visualization server's I/O thread
// and only enqueues. Everything else - validation, the blocking Capital.com
// login, starting and joining workers - happens one command at a time on the
// manager's own controller thread, so a Disconnect can never race a Start.
// ---------------------------------------------------------------------------
namespace app {

enum class Market { Binance, Capital };
const char* marketId(Market market);  // "binance" / "capital"
optional<Market> parseMarket(const string& id);

enum class Phase { Idle, Starting, Running, Stopping };
const char* phaseName(Phase phase);

// The market-specific pieces, injected so the manager can be tested without a
// network (see tests/session_manager_test.cpp) and so session_manager.cpp does
// not depend on binance.cpp. main.cpp supplies the real ones.
struct Backends {
    // Optional pre-check of a Binance symbol; false + errorText rejects the start.
    function<bool(const string& symbol, string& errorText)> checkBinanceSymbol;

    // Run the Binance workers; return once they have all ended - which they do
    // when `stop` fires (or by themselves if every one of them fails).
    function<void(const string& symbol, const vector<pipeline::TimeframeTask>& tasks,
                  const filesystem::path& folder, pipeline::TerminalBoard& board,
                  visualization::VisualizationServer& viz, StopSignal& stop)>
        runBinance;

    // Capital.com: log in and check the epic (nullptr + errorText on failure), then run.
    function<shared_ptr<capital::Session>(const string& epic, string& errorText)> capitalPreflight;
    function<void(shared_ptr<capital::Session> session, const string& epic,
                  const vector<pipeline::TimeframeTask>& tasks, const filesystem::path& folder,
                  pipeline::TerminalBoard& board, visualization::VisualizationServer& viz,
                  StopSignal& stop)>
        runCapital;
};

class SessionManager {
public:
    // Registers itself as `viz`'s command handler and publishes the initial
    // "idle" state - so construct it BEFORE viz.start(). CSV folders are created
    // under `dataRoot` (the working directory in production).
    SessionManager(visualization::VisualizationServer& viz, Backends backends,
                   filesystem::path dataRoot = ".");
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Thread-safe and non-blocking: parses one control message from the browser
    // and queues it. Anything malformed or unknown is ignored.
    void handleMessage(const string& text);

    // Current phase; safe from any thread.
    Phase phase() const { return phase_.load(); }

    // Ends any running session and stops the controller thread. Idempotent;
    // also run by the destructor.
    void shutdown();

private:
    struct Command {
        enum class Kind { Start, Disconnect, SessionEnded, Quit };
        Kind kind = Kind::Quit;
        string market;
        string symbol;
        string timeframes;
        uint64_t sessionId = 0;
    };

    struct Active {
        uint64_t id = 0;
        Market market = Market::Binance;
        string symbol;
        vector<string> labels;
        vector<pipeline::TimeframeTask> tasks;
        filesystem::path folder;
        shared_ptr<capital::Session> capitalSession;
        StopSignal stop;
        unique_ptr<pipeline::TerminalBoard> board;
        thread runner;
    };

    void enqueue(Command command);
    void controllerLoop();
    void onStart(const Command& command);
    void teardown(const string& errorForIdle);
    void rejectStart(optional<Market> market, const string& error);

    void publish(Phase phase, optional<Market> market, const string& symbol,
                 const vector<string>& timeframes, const string& error,
                 const vector<string>& warnings);
    void publishIdle();

    visualization::VisualizationServer& viz_;
    Backends backends_;
    filesystem::path dataRoot_;

    mutex mutex_;
    condition_variable cv_;
    deque<Command> queue_;
    bool shutdownStarted_ = false;
    thread controller_;

    atomic<Phase> phase_{Phase::Idle};
    uint64_t nextSessionId_ = 1;           // controller thread only
    unique_ptr<Active> active_;            // controller thread only
    string lastStateJson_;                 // controller thread only
};

}  // namespace app