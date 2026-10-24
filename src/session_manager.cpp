#include "session_manager.h"

#include "utils/candlesticks.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <system_error>

using json = nlohmann::json;
using namespace std;

namespace app {
namespace {

constexpr size_t kMaxSymbolChars = 40;       // Capital's isValidEpic() caps at 40; Binance symbols are shorter
constexpr size_t kMaxTimeframeChars = 256;   // 15 distinct timeframes fit in well under 100
constexpr size_t kMaxPendingCommands = 64;   // a runaway page cannot grow the queue without bound

// Echoing what the browser typed into an error message must not be able to
// smuggle control characters into the terminal or an over-long string into the
// reply: printable ASCII only, truncated.
string sanitizeForDisplay(const string& raw) {
    string out;
    for (const unsigned char c : raw) {
        if (out.size() >= 40) {
            out += "...";
            break;
        }
        out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return out;
}

bool isValidBinanceSymbol(const string& symbol) {
    if (symbol.empty() || symbol.size() > kMaxSymbolChars) {
        return false;
    }
    for (const char c : symbol) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* marketId(Market market) { return market == Market::Binance ? "binance" : "capital"; }

optional<Market> parseMarket(const string& id) {
    const string lowered = pipeline::toLower(pipeline::trim(id));
    if (lowered == "binance") return Market::Binance;
    if (lowered == "capital") return Market::Capital;
    return nullopt;
}

const char* phaseName(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "idle";
        case Phase::Starting: return "starting";
        case Phase::Running: return "running";
        case Phase::Stopping: return "stopping";
    }
    return "idle";
}

SessionManager::SessionManager(visualization::VisualizationServer& viz, Backends backends,
                               filesystem::path dataRoot)
    : viz_(viz), backends_(move(backends)), dataRoot_(move(dataRoot)) {
    publishIdle();
    viz_.setCommandHandler([this](const string& text) { handleMessage(text); });
    controller_ = thread([this] { controllerLoop(); });
}

SessionManager::~SessionManager() { shutdown(); }

void SessionManager::shutdown() {
    // After this returns no handleMessage() call is in flight or can start.
    viz_.setCommandHandler(nullptr);
    {
        lock_guard<mutex> lock(mutex_);
        if (!shutdownStarted_) {
            shutdownStarted_ = true;
            queue_.clear();  // whatever the browser had queued is moot now
            Command quit;
            quit.kind = Command::Kind::Quit;
            queue_.push_back(move(quit));
        }
    }
    cv_.notify_all();
    if (controller_.joinable()) {
        controller_.join();
    }
}

// ---------------------------------------------------------------------------
// Incoming messages (visualization I/O thread)
// ---------------------------------------------------------------------------
void SessionManager::handleMessage(const string& text) {
    const json message = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!message.is_object()) {
        return;
    }
    const auto field = [&message](const char* key) -> string {
        const auto it = message.find(key);
        return (it != message.end() && it->is_string()) ? it->get<string>() : string();
    };

    const string type = field("type");
    Command command;
    if (type == "start") {
        command.kind = Command::Kind::Start;
        command.market = field("market");
        command.symbol = field("symbol");
        command.timeframes = field("timeframes");
    } else if (type == "disconnect") {
        command.kind = Command::Kind::Disconnect;
    } else {
        return;
    }
    enqueue(move(command));
}

void SessionManager::enqueue(Command command) {
    {
        lock_guard<mutex> lock(mutex_);
        if (shutdownStarted_) {
            return;
        }
        const bool fromBrowser = command.kind == Command::Kind::Start ||
                                 command.kind == Command::Kind::Disconnect;
        if (fromBrowser && queue_.size() >= kMaxPendingCommands) {
            return;
        }
        queue_.push_back(move(command));
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Controller thread: one command at a time
// ---------------------------------------------------------------------------
void SessionManager::controllerLoop() {
    for (;;) {
        Command command;
        {
            unique_lock<mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty(); });
            command = move(queue_.front());
            queue_.pop_front();
        }

        switch (command.kind) {
            case Command::Kind::Start:
                onStart(command);
                break;
            case Command::Kind::Disconnect:
                if (active_) {
                    teardown("");
                } else {
                    viz_.setSessionState(lastStateJson_);  // already idle: just re-sync the page
                }
                break;
            case Command::Kind::SessionEnded:
                // Only meaningful for the session that is still active; the one a
                // Disconnect just tore down reports in after the fact and is ignored.
                if (active_ && active_->id == command.sessionId) {
                    teardown("The session ended unexpectedly - see the terminal for details.");
                }
                break;
            case Command::Kind::Quit:
                teardown("");
                return;
        }
    }
}

void SessionManager::onStart(const Command& command) {
    if (active_) {
        viz_.setSessionState(lastStateJson_);  // a second tab raced this one: tell it what is running
        return;
    }

    const auto market = parseMarket(command.market);
    if (!market) {
        rejectStart(nullopt, "Choose a market first.");
        return;
    }
    const bool binance = *market == Market::Binance;

    // ---- validate what was typed (nothing is started or touched on disk yet) ----
    const string symbol = pipeline::toUpper(pipeline::trim(command.symbol));
    if (symbol.empty()) {
        rejectStart(market, binance ? "Symbol must not be empty." : "Epic must not be empty.");
        return;
    }
    const bool symbolOk = binance ? isValidBinanceSymbol(symbol) : capital::isValidEpic(symbol);
    if (!symbolOk) {
        rejectStart(market, string("Invalid ") + (binance ? "symbol" : "epic") + " '" +
                                sanitizeForDisplay(symbol) + "': " +
                                (binance ? "use capital letters, digits and '_' only."
                                         : "use letters, digits, '.', '_' or '-' only."));
        return;
    }
    if (command.timeframes.size() > kMaxTimeframeChars) {
        rejectStart(market, "The timeframe list is too long.");
        return;
    }
    vector<string> warnings;
    vector<pipeline::TimeframeTask> tasks = pipeline::buildTasks(command.timeframes, &warnings);
    for (const auto& warning : warnings) {
        cerr << warning << endl;
    }
    if (tasks.empty()) {
        rejectStart(market,
                    "No valid timeframes provided. " +
                        (warnings.empty() ? string("Enter them comma-separated, e.g. 1m,15m,1h,1d.")
                                          : warnings.front()));
        return;
    }
    vector<string> labels;
    labels.reserve(tasks.size());
    for (const auto& task : tasks) {
        labels.push_back(task.rawTimeframe);
    }

    // ---- network checks (these can take a few seconds: the page shows "starting") ----
    publish(Phase::Starting, market, symbol, labels, "", warnings);

    shared_ptr<capital::Session> capitalSession;
    string errorText;
    if (binance) {
        if (backends_.checkBinanceSymbol && !backends_.checkBinanceSymbol(symbol, errorText)) {
            rejectStart(market, errorText.empty() ? "Unknown symbol '" + symbol + "'." : errorText);
            return;
        }
    } else {
        // Done BEFORE the TerminalBoard exists, as in the old CLI: a missing
        // credential or a mistyped epic is plain text, not a board fight.
        capitalSession = backends_.capitalPreflight ? backends_.capitalPreflight(symbol, errorText)
                                                    : nullptr;
        if (!capitalSession) {
            rejectStart(market, errorText.empty() ? "Could not log in to Capital.com." : errorText);
            return;
        }
    }

    // Capital.com data lives in fx_<epic>/, never next to the Binance symbol
    // folders: a Binance symbol such as "eurusdt" could otherwise collide with a
    // Forex epic of the same lower-cased name.
    const string folderName = (binance ? "" : "fx_") + pipeline::toLower(symbol);
    const filesystem::path folder = dataRoot_ / folderName;
    error_code dirError;
    filesystem::create_directories(folder, dirError);
    if (dirError) {
        rejectStart(market, "Failed to create folder '" + folderName + "': " + dirError.message());
        return;
    }

    cout << "Streaming " << symbol << " across " << tasks.size()
         << " timeframe(s). Closed candles are appended to CSVs under '" << folderName
         << "/'. Press Disconnect in the browser to switch symbol, or Ctrl+C to exit.\n";

    auto active = make_unique<Active>();
    active->id = nextSessionId_++;
    active->market = *market;
    active->symbol = symbol;
    active->labels = labels;
    active->tasks = move(tasks);
    active->folder = folder;
    active->capitalSession = move(capitalSession);
    active->board = make_unique<pipeline::TerminalBoard>(labels);
    active_ = move(active);

    // "running" is published BEFORE any worker exists, so the page has switched to
    // the chart screen - and will accept this symbol's data - before the first
    // history or candle message can possibly be sent.
    publish(Phase::Running, market, symbol, labels, "", warnings);

    try {
        active_->runner = thread([this, a = active_.get()] {
            try {
                if (a->market == Market::Binance) {
                    if (backends_.runBinance) {
                        backends_.runBinance(a->symbol, a->tasks, a->folder, *a->board, viz_, a->stop);
                    }
                } else if (backends_.runCapital) {
                    backends_.runCapital(a->capitalSession, a->symbol, a->tasks, a->folder, *a->board,
                                         viz_, a->stop);
                }
            } catch (const exception& e) {
                a->board->log(string("Session error: ") + e.what());
            } catch (...) {
                a->board->log("Session error: unknown exception");
            }
            // Last act: lets the controller notice a session that ended on its own.
            Command ended;
            ended.kind = Command::Kind::SessionEnded;
            ended.sessionId = a->id;
            enqueue(move(ended));
        });
    } catch (const system_error& e) {
        active_.reset();
        viz_.resetSessionData();
        rejectStart(market, string("Could not start the session threads: ") + e.what());
    }
}

void SessionManager::teardown(const string& errorForIdle) {
    if (!active_) {
        return;
    }
    const Market market = active_->market;
    publish(Phase::Stopping, market, active_->symbol, active_->labels, "", {});
    active_->board->log("Disconnecting " + active_->symbol + "...");

    active_->stop.request();
    if (active_->runner.joinable()) {
        active_->runner.join();  // returns once every worker has stopped
    }
    active_.reset();             // the TerminalBoard goes with it
    viz_.resetSessionData();     // the next symbol must not inherit this one's candles

    cout << "Disconnected. Choose a market in the browser to start again, or press Ctrl+C to exit.\n";
    if (errorForIdle.empty()) {
        publishIdle();
    } else {
        cerr << errorForIdle << endl;
        publish(Phase::Idle, market, "", {}, errorForIdle, {});
    }
}

void SessionManager::rejectStart(optional<Market> market, const string& error) {
    cerr << error << endl;
    publish(Phase::Idle, market, "", {}, error, {});
}

// ---------------------------------------------------------------------------
// State publishing
// ---------------------------------------------------------------------------
void SessionManager::publishIdle() { publish(Phase::Idle, nullopt, "", {}, "", {}); }

void SessionManager::publish(Phase phase, optional<Market> market, const string& symbol,
                             const vector<string>& timeframes, const string& error,
                             const vector<string>& warnings) {
    json state;
    state["type"] = "session_state";
    state["state"] = phaseName(phase);
    if (market) {
        state["market"] = marketId(*market);
    }
    if (!symbol.empty()) {
        state["symbol"] = symbol;
    }
    if (!timeframes.empty()) {
        state["timeframes"] = timeframes;
    }
    if (!error.empty()) {
        state["error"] = error;
    }
    if (!warnings.empty()) {
        state["warnings"] = warnings;
    }
    phase_.store(phase);
    // `replace`, not the default `strict`: a warning that echoes odd bytes must never
    // make dump() throw and take the controller thread down with it.
    lastStateJson_ = state.dump(-1, ' ', false, json::error_handler_t::replace);
    viz_.setSessionState(lastStateJson_);
}

}  // namespace app