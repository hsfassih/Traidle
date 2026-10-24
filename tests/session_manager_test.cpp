// ---------------------------------------------------------------------------
// session_manager_test: offline test of the browser-driven session switching
// (market picker -> symbol/timeframes -> Disconnect -> picker again).
//
// It runs the REAL VisualizationServer and the REAL SessionManager, but with
// fake market backends (no network, no credentials): a fake "worker" per
// timeframe seeds some history, publishes it and then streams forming candles
// until it is told to stop, exactly like the real workers' contract. A real
// WebSocket client plays the browser and drives the full command protocol.
//
//   build:  cmake -DTRAIDLE_BUILD_TESTS=ON ... && cmake --build . --target session_manager_test
//   run:    ./session_manager_test              (about 30 s; exit code 0 = all checks passed)
//           ./session_manager_test --serve [port]   keep the fake-backed app running for manual
//                                                    browser testing (default port 8765)
//
// POSIX only, like capital_mock_test.
// ---------------------------------------------------------------------------

#include "../src/capital/capital_api.h"
#include "../src/utils/pipeline_common.h"
#include "../src/session_manager.h"
#include "../src/visualization.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Tiny test harness (same style as the other tests)
// ---------------------------------------------------------------------------
static int gChecks = 0;
static int gFailed = 0;

static void check(bool ok, const string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailed;
        printf("  FAIL: %s\n", what.c_str());
        fflush(stdout);
    }
}
#define CHECK(cond) check((cond), #cond)
#define CHECK_MSG(cond, msg) check((cond), (msg))

static void section(const string& title) {
    printf("\n== %s\n", title.c_str());
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// The fake market world
// ---------------------------------------------------------------------------
namespace {

int64_t nowMs() { return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); }

int64_t stepMsFor(const string& binanceInterval) {
    const int64_t n = atoll(binanceInterval.c_str());
    switch (binanceInterval.back()) {
        case 'm': return n * 60 * 1000;
        case 'h': return n * 3600 * 1000;
        case 'd': return n * 86400 * 1000;
        case 'w': return n * 7 * 86400 * 1000;
        default: return n * 30 * 86400 * 1000;  // 'M'
    }
}

visualization::VisualizationMessage makeCandle(const string& symbol, const string& label,
                                               int64_t openTime, int64_t step, double base, int i,
                                               bool closed) {
    visualization::VisualizationMessage m;
    m.symbol = symbol;
    m.timeframe = label;
    m.openTime = openTime;
    m.closeTime = openTime + step - 1;
    m.closed = closed;
    m.open = base * (1.0 + 0.01 * sin(i * 0.31));
    m.close = base * (1.0 + 0.01 * sin((i + 1) * 0.31));
    m.high = max(m.open, m.close) * 1.002;
    m.low = min(m.open, m.close) * 0.998;
    m.baseVolume = 100.0 + 50.0 * sin(i * 0.17);
    m.quoteVolume = m.baseVolume * base;
    m.takerBuyBaseVolume = m.baseVolume * 0.55;
    m.takerBuyQuoteVolume = m.quoteVolume * 0.55;
    if (closed) {
        m.indicatorValues.sma = base;
        m.indicatorValues.ema = base * 1.001;
        m.indicatorValues.bbUpper = base * 1.02;
        m.indicatorValues.bbLower = base * 0.98;
    }
    return m;
}

struct FakeWorld {
    mutex m;
    vector<string> started;      // "binance:BTCUSDT", "capital:EURUSD" ... in start order
    atomic<int> liveWorkers{0};  // fake timeframe workers currently running
    atomic<int> runnersEnded{0};

    size_t startedCount() {
        lock_guard<mutex> lock(m);
        return started.size();
    }
};

// One fake timeframe worker: the same contract as the real ones - seed history, publish it,
// stream forming candles, and return promptly once `stop` fires.
void fakeTimeframeWorker(visualization::VisualizationServer& viz, FakeWorld& world,
                         const string& symbol, const pipeline::TimeframeTask& task, double base,
                         app::StopSignal& stop) {
    ++world.liveWorkers;
    const string label = task.rawTimeframe;
    const int64_t step = stepMsFor(task.binanceInterval);
    const int64_t current = nowMs() - nowMs() % step;
    for (int i = 300; i >= 1; --i) {
        viz.seedHistory(makeCandle(symbol, label, current - i * step, step, base, 300 - i, true));
    }
    viz.setSmcSnapshot(symbol, label, smc::SmcSnapshot{});
    viz.publishTimeframe(label);
    int tick = 0;
    while (!stop.waitFor(milliseconds(200))) {
        viz.push(makeCandle(symbol, label, current, step, base, 300 + (++tick % 7), false));
    }
    --world.liveWorkers;
}

// `tag` ("binance:BTCUSDT") only goes into world.started; the chart messages carry the bare
// symbol, exactly like the real workers (the page gates incoming data on it).
void runFakeFeed(visualization::VisualizationServer& viz, FakeWorld& world, const string& tag,
                 const string& symbol, const vector<pipeline::TimeframeTask>& tasks, double base,
                 app::StopSignal& stop) {
    {
        lock_guard<mutex> lock(world.m);
        world.started.push_back(tag);
    }
    if (symbol == "DIESOON") {
        this_thread::sleep_for(milliseconds(300));  // a session whose workers all fail by themselves
        ++world.runnersEnded;
        return;
    }
    vector<thread> workers;
    for (const auto& task : tasks) {
        workers.emplace_back(fakeTimeframeWorker, ref(viz), ref(world), symbol, task, base, ref(stop));
    }
    for (auto& worker : workers) {
        worker.join();
    }
    ++world.runnersEnded;
}

app::Backends fakeBackends(FakeWorld& world) {
    app::Backends backends;
    backends.checkBinanceSymbol = [](const string& symbol, string& error) {
        if (symbol == "BADSYM") {
            error = "Unknown Binance USD-M Futures symbol 'BADSYM'";
            return false;
        }
        return true;
    };
    backends.runBinance = [&world](const string& symbol, const vector<pipeline::TimeframeTask>& tasks,
                                   const filesystem::path&, pipeline::TerminalBoard&,
                                   visualization::VisualizationServer& viz, app::StopSignal& stop) {
        runFakeFeed(viz, world, "binance:" + symbol, symbol, tasks, 60000.0, stop);
    };
    backends.capitalPreflight = [](const string& epic, string& error) -> shared_ptr<capital::Session> {
        if (epic == "NOPE") {
            error = "Unknown epic 'NOPE' on Capital.com";
            return nullptr;
        }
        if (epic == "NOLOGIN") {
            error = "Capital.com login failed (HTTP 401)";
            return nullptr;
        }
        if (epic == "SLOWFX") {
            this_thread::sleep_for(milliseconds(800));
        }
        return make_shared<capital::Session>(capital::Credentials{}, capital::Endpoints{});
    };
    backends.runCapital = [&world](shared_ptr<capital::Session>, const string& epic,
                                   const vector<pipeline::TimeframeTask>& tasks,
                                   const filesystem::path&, pipeline::TerminalBoard&,
                                   visualization::VisualizationServer& viz, app::StopSignal& stop) {
        runFakeFeed(viz, world, "capital:" + epic, epic, tasks, 1.08, stop);
    };
    return backends;
}

}  // namespace

// ---------------------------------------------------------------------------
// A WebSocket client playing the browser. All socket operations run on its own I/O thread.
// ---------------------------------------------------------------------------
class WsClient {
public:
    explicit WsClient(unsigned short port, const string& origin = "") : ws_(ioc_) {
        tcp::resolver resolver(ioc_);
        beast::get_lowest_layer(ws_).connect(resolver.resolve("127.0.0.1", to_string(port)));
        ws_.set_option(websocket::stream_base::decorator([origin](websocket::request_type& req) {
            if (!origin.empty()) {
                req.set(http::field::origin, origin);
            }
        }));
        ws_.read_message_max(256 * 1024 * 1024);
        ws_.handshake("127.0.0.1:" + to_string(port), "/");
        net::post(ioc_, [this] { doRead(); });
        thread_ = thread([this] { ioc_.run(); });
    }

    ~WsClient() {
        net::post(ioc_, [this] { beast::close_socket(beast::get_lowest_layer(ws_)); });
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void send(const string& text) {
        net::post(ioc_, [this, text] {
            outbox_.push_back(text);
            if (!writing_) {
                doWrite();
            }
        });
    }

    // Waits for the oldest message not yet consumed that satisfies `pred`, and consumes it.
    // Messages that do not match stay available to later calls (two timeframes' histories may
    // arrive in either order). Returns a null json on timeout.
    json waitFor(const function<bool(const json&)>& pred, int timeoutMs = 8000) {
        unique_lock<mutex> lock(m_);
        json found;
        const bool ok = cv_.wait_for(lock, milliseconds(timeoutMs), [&] {
            for (size_t i = 0; i < all_.size(); ++i) {
                if (!consumed_[i] && pred(all_[i])) {
                    consumed_[i] = true;
                    found = all_[i];
                    return true;
                }
            }
            return false;
        });
        return ok ? found : json();
    }

    json waitState(const string& state, int timeoutMs = 8000) {
        return waitFor(
            [&](const json& m) { return m.value("type", "") == "session_state" && m.value("state", "") == state; },
            timeoutMs);
    }

    // Waits `ms`, then reports whether an unconsumed message satisfying `pred` is present
    // (it is not consumed).
    bool sees(const function<bool(const json&)>& pred, int ms) {
        this_thread::sleep_for(milliseconds(ms));
        lock_guard<mutex> lock(m_);
        for (size_t i = 0; i < all_.size(); ++i) {
            if (!consumed_[i] && pred(all_[i])) return true;
        }
        return false;
    }

    vector<json> all() {
        lock_guard<mutex> lock(m_);
        return all_;
    }
    size_t count() {
        lock_guard<mutex> lock(m_);
        return all_.size();
    }
    // Marks everything received so far as consumed (a clean slate between scenarios).
    void skipToEnd() {
        lock_guard<mutex> lock(m_);
        consumed_.assign(all_.size(), true);
    }
    bool closed() {
        lock_guard<mutex> lock(m_);
        return closed_;
    }

private:
    void doRead() {
        ws_.async_read(buffer_, [this](beast::error_code ec, size_t) {
            if (ec) {
                lock_guard<mutex> lock(m_);
                closed_ = true;
                cv_.notify_all();
                return;
            }
            json message = json::parse(beast::buffers_to_string(buffer_.data()), nullptr, false);
            buffer_.consume(buffer_.size());
            {
                lock_guard<mutex> lock(m_);
                all_.push_back(move(message));
                consumed_.push_back(false);
            }
            cv_.notify_all();
            doRead();
        });
    }

    void doWrite() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(outbox_.front()), [this](beast::error_code ec, size_t) {
            outbox_.pop_front();
            if (ec || outbox_.empty()) {
                writing_ = false;
                return;
            }
            doWrite();
        });
    }

    net::io_context ioc_;
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    deque<string> outbox_;
    bool writing_ = false;
    thread thread_;
    mutex m_;
    condition_variable cv_;
    vector<json> all_;
    vector<char> consumed_;
    bool closed_ = false;
};

static string startCommand(const string& market, const string& symbol, const string& timeframes) {
    return json{{"type", "start"}, {"market", market}, {"symbol", symbol}, {"timeframes", timeframes}}.dump();
}
static const string kDisconnect = R"({"type":"disconnect"})";

static bool isHistoryFor(const json& m, const string& symbol, const string& timeframe) {
    return m.value("type", "") == "history" && m.value("symbol", "") == symbol &&
           m.value("timeframe", "") == timeframe;
}

static bool waitUntil(const function<bool()>& cond, int timeoutMs) {
    const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    while (steady_clock::now() < deadline) {
        if (cond()) return true;
        this_thread::sleep_for(milliseconds(20));
    }
    return cond();
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool serve = false;
    unsigned short port = 18770;
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--serve") {
            serve = true;
            port = 8765;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                port = static_cast<unsigned short>(atoi(argv[++i]));
            }
        }
    }

    const filesystem::path scratch =
        filesystem::temp_directory_path() / ("session_manager_test_" + to_string(getpid()));
    filesystem::create_directories(scratch);

    FakeWorld world;
    visualization::VisualizationServer viz(port);
    app::SessionManager manager(viz, fakeBackends(world), scratch);
    viz.start();
    if (!viz.isRunning()) {
        printf("cannot bind port %u\n", port);
        return 2;
    }
    if (serve) {
        printf("session_manager_test: serving the fake-backed app on ws://127.0.0.1:%u (Ctrl+C to stop)\n", port);
        fflush(stdout);
        for (;;) this_thread::sleep_for(hours(24));
    }

    // Everything below prints through printf; the board/server chatter goes to the same terminal.
    // ---------------------------------------------------------------------
    section("1. originAllowsCommands");
    for (const char* ok : {"", "null", "http://localhost", "http://localhost:8080", "https://127.0.0.1:3000",
                           "http://[::1]:80", "HTTP://LOCALHOST:1"}) {
        CHECK_MSG(visualization::originAllowsCommands(ok), string("allowed: '") + ok + "'");
    }
    for (const char* bad : {"http://evil.example", "https://localhost.evil.example", "http://127.0.0.1.evil.example",
                            "file://", "garbage", "http://", "http://[::1", "https://example.com:8765"}) {
        CHECK_MSG(!visualization::originAllowsCommands(bad), string("refused: '") + bad + "'");
    }

    // ---------------------------------------------------------------------
    section("2. A fresh viewer starts on the picker (idle), and bad input is rejected with a reason");
    WsClient a(port);
    {
        json firstMessage = a.waitFor([](const json&) { return true; }, 3000);
        CHECK(!firstMessage.is_null());
        CHECK(firstMessage.value("type", "") == "session_state");
        CHECK(firstMessage.value("state", "") == "idle");
        CHECK(!firstMessage.contains("error"));
    }
    // Garbage is ignored silently (no state change, no reply).
    a.send("this is not json");
    a.send("[1,2,3]");
    a.send(R"({"type":"nonsense"})");
    a.send(R"({"no_type":true})");
    CHECK(!a.sees([](const json& m) { return m.value("type", "") == "session_state"; }, 300));
    CHECK(manager.phase() == app::Phase::Idle);
    // A start whose fields have the wrong JSON types counts as an incomplete form: it is answered.
    a.send(R"({"type":"start","market":5,"symbol":["x"]})");
    {
        const json idle = a.waitFor(
            [](const json& m) { return m.value("state", "") == "idle" && m.contains("error"); }, 3000);
        CHECK(!idle.is_null());
        if (!idle.is_null()) CHECK(idle.value("error", "").find("Choose a market") != string::npos);
    }

    struct Rejection { const char* market; const char* symbol; const char* timeframes; const char* expect; const char* shownMarket; };
    const Rejection rejections[] = {
        {"", "BTCUSDT", "1m", "Choose a market first.", nullptr},
        {"bitcoin", "BTCUSDT", "1m", "Choose a market first.", nullptr},
        {"binance", "   ", "1m", "Symbol must not be empty.", "binance"},
        {"binance", "BTC USDT", "1m", "Invalid symbol", "binance"},
        {"binance", "BTC/USDT", "1m", "Invalid symbol", "binance"},
        {"capital", "", "1m", "Epic must not be empty.", "capital"},
        {"capital", "EUR/USD", "1m", "Invalid epic", "capital"},
        {"binance", "BTCUSDT", "", "No valid timeframes provided", "binance"},
        {"binance", "BTCUSDT", "zz, 7x", "No valid timeframes provided", "binance"},
        {"binance", "BADSYM", "1m", "Unknown Binance USD-M Futures symbol 'BADSYM'", "binance"},
        {"capital", "NOPE", "1m", "Unknown epic 'NOPE' on Capital.com", "capital"},
        {"capital", "NOLOGIN", "1m", "Capital.com login failed", "capital"},
    };
    for (const auto& r : rejections) {
        a.send(startCommand(r.market, r.symbol, r.timeframes));
        const json idle = a.waitFor(
            [](const json& m) {
                return m.value("type", "") == "session_state" && m.value("state", "") == "idle" && m.contains("error");
            },
            5000);
        const string label = string("[") + r.market + "/" + r.symbol + "/" + r.timeframes + "] ";
        CHECK_MSG(!idle.is_null(), label + "answered with idle + error");
        if (!idle.is_null()) {
            CHECK_MSG(idle.value("error", "").find(r.expect) != string::npos,
                      label + "error mentions '" + r.expect + "' (got '" + idle.value("error", "") + "')");
            if (r.shownMarket != nullptr) {
                CHECK_MSG(idle.value("market", "") == r.shownMarket, label + "keeps the chosen market for the form");
            } else {
                CHECK_MSG(!idle.contains("market"), label + "no market named");
            }
        }
        CHECK(manager.phase() == app::Phase::Idle);
    }
    CHECK_MSG(world.startedCount() == 0, "no rejected request started a feed");

    // ---------------------------------------------------------------------
    section("3. Start a Binance session: starting -> running, history reaches the ALREADY connected page");
    a.send(startCommand("binance", " btcusdt ", "1m, 15m, 1m, zz"));
    {
        const json starting = a.waitState("starting", 5000);
        CHECK(!starting.is_null());
        const json running = a.waitState("running", 5000);
        CHECK(!running.is_null());
        CHECK(running.value("market", "") == "binance");
        CHECK(running.value("symbol", "") == "BTCUSDT");
        CHECK(running.value("timeframes", json::array()) == json::array({"1m", "15m"}));
        const auto warnings = running.value("warnings", json::array());
        CHECK_MSG(warnings.size() == 2, "the duplicate and the unsupported timeframe are reported as warnings");

        const json h1 = a.waitFor([](const json& m) { return isHistoryFor(m, "BTCUSDT", "1m"); }, 5000);
        const json h15 = a.waitFor([](const json& m) { return isHistoryFor(m, "BTCUSDT", "15m"); }, 5000);
        CHECK_MSG(!h1.is_null() && !h15.is_null(), "a history message per timeframe arrives without reconnecting");
        if (!h1.is_null()) CHECK(h1["candles"].size() == 300);
        const json smcSnapshot = a.waitFor(
            [](const json& m) { return m.value("type", "") == "smc_snapshot" && m.value("timeframe", "") == "15m"; }, 3000);
        CHECK(!smcSnapshot.is_null());
        const json live = a.waitFor(
            [](const json& m) { return m.value("type", "") == "candle" && m.value("symbol", "") == "BTCUSDT"; }, 3000);
        CHECK_MSG(!live.is_null(), "live candles follow");
    }
    CHECK(manager.phase() == app::Phase::Running);
    CHECK(world.liveWorkers == 2);
    CHECK(filesystem::exists(scratch / "btcusdt"));

    // ---------------------------------------------------------------------
    section("4. A second tab joins mid-session: state first, then history; a stray start is ignored");
    {
        WsClient b(port);
        const json first = b.waitFor([](const json&) { return true; }, 3000);
        CHECK(first.value("type", "") == "session_state");
        CHECK(first.value("state", "") == "running");
        CHECK(first.value("symbol", "") == "BTCUSDT");
        CHECK(!b.waitFor([](const json& m) { return isHistoryFor(m, "BTCUSDT", "1m"); }, 5000).is_null());

        const size_t startedBefore = world.startedCount();
        b.send(startCommand("capital", "EURUSD", "1m"));
        const json again = b.waitState("running", 3000);
        CHECK_MSG(!again.is_null() && again.value("symbol", "") == "BTCUSDT", "the page is told what is really running");
        CHECK(world.startedCount() == startedBefore);
        CHECK(manager.phase() == app::Phase::Running);
    }

    // ---------------------------------------------------------------------
    section("5. Disconnect: stopping -> idle, workers really stop, no stale data afterwards");
    a.skipToEnd();
    const auto t0 = steady_clock::now();
    a.send(kDisconnect);
    CHECK(!a.waitState("stopping", 3000).is_null());
    CHECK(!a.waitState("idle", 8000).is_null());
    const auto stopMs = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    printf("   (Disconnect took %lld ms)\n", static_cast<long long>(stopMs));
    CHECK_MSG(stopMs < 3000, "Disconnect completes promptly");
    CHECK(manager.phase() == app::Phase::Idle);
    CHECK(world.liveWorkers == 0);
    a.skipToEnd();
    CHECK_MSG(!a.sees([](const json& m) { return m.value("type", "") != "session_state" && m.value("type", "") != "heartbeat"; }, 700),
              "nothing from the old session arrives after idle");
    {
        WsClient c(port);
        const json first = c.waitFor([](const json&) { return true; }, 3000);
        CHECK(first.value("state", "") == "idle");
        CHECK_MSG(!c.sees([](const json& m) { return m.value("type", "") == "history" || m.value("type", "") == "candle" ||
                                                      m.value("type", "") == "smc_snapshot"; }, 600),
                  "a page that connects between sessions gets no leftover chart data");
    }
    a.send(kDisconnect);  // Disconnect while idle is harmless
    CHECK(!a.waitState("idle", 3000).is_null());

    // ---------------------------------------------------------------------
    section("6. A different market and symbol in the same process; data of the previous symbol never reappears");
    a.skipToEnd();
    a.send(startCommand("capital", "eurusd", "1h"));
    {
        const json running = a.waitState("running", 8000);
        CHECK(!running.is_null());
        CHECK(running.value("market", "") == "capital");
        CHECK(running.value("symbol", "") == "EURUSD");
        CHECK(!a.waitFor([](const json& m) { return isHistoryFor(m, "EURUSD", "1h"); }, 5000).is_null());
        a.skipToEnd();
        CHECK_MSG(!a.sees([](const json& m) { return m.value("symbol", "") == "BTCUSDT"; }, 600),
                  "no BTCUSDT message in the EURUSD session");
    }
    CHECK(filesystem::exists(scratch / "fx_eurusd"));
    a.send(kDisconnect);
    CHECK(!a.waitState("idle", 8000).is_null());
    CHECK(world.liveWorkers == 0);

    // ---------------------------------------------------------------------
    section("7. Only localhost / file pages may send commands");
    {
        WsClient evil(port, "http://evil.example");
        CHECK(!evil.waitFor([](const json&) { return true; }, 3000).is_null());  // it may watch...
        evil.send(startCommand("binance", "ETHUSDT", "1m"));
        CHECK_MSG(!evil.sees([](const json& m) { return m.value("type", "") == "session_state" && m.value("state", "") != "idle"; }, 800),
                  "...but its start command is ignored");
        CHECK(manager.phase() == app::Phase::Idle);
        CHECK(world.liveWorkers == 0);
    }
    {
        WsClient local(port, "http://localhost:5500");
        CHECK(!local.waitFor([](const json&) { return true; }, 3000).is_null());
        local.send(startCommand("binance", "ETHUSDT", "5m"));
        CHECK(!local.waitState("running", 8000).is_null());
        {
            WsClient evil(port, "https://localhost.evil.example");
            evil.waitFor([](const json&) { return true; }, 3000);
            evil.send(kDisconnect);
            CHECK_MSG(!evil.sees([](const json& m) { return m.value("state", "") == "stopping"; }, 800), "a foreign page cannot disconnect either");
            CHECK(manager.phase() == app::Phase::Running);
        }
        local.send(kDisconnect);
        CHECK(!local.waitState("idle", 8000).is_null());
    }

    // ---------------------------------------------------------------------
    section("8. Oversized frames are refused");
    {
        WsClient big(port);
        big.waitFor([](const json&) { return true; }, 3000);
        big.send(string(200 * 1024, 'x'));
        CHECK_MSG(waitUntil([&] { return big.closed(); }, 3000), "the server drops a client that sends a 200 KB frame");
        CHECK(manager.phase() == app::Phase::Idle);
    }

    // ---------------------------------------------------------------------
    section("9. Rapid start/disconnect cycles, a Disconnect that lands during a slow login, a session that dies by itself");
    a.skipToEnd();
    const size_t cyclesBase = world.startedCount();
    for (int i = 0; i < 4; ++i) {
        a.send(startCommand("binance", i % 2 ? "SOLUSDT" : "ADAUSDT", "1m,5m,15m"));
        a.send(kDisconnect);  // queued right behind the start: processed once the start finished
    }
    CHECK_MSG(waitUntil([&] {
                  return manager.phase() == app::Phase::Idle && world.startedCount() == cyclesBase + 4 &&
                         static_cast<size_t>(world.runnersEnded.load()) == world.startedCount();
              }, 20000),
              "4 start+disconnect pairs each start once and end idle");
    CHECK(world.liveWorkers == 0);
    {
        const size_t before = world.startedCount();
        this_thread::sleep_for(milliseconds(400));  // let the last state messages of the cycles arrive
        a.skipToEnd();
        a.send(startCommand("capital", "SLOWFX", "1m"));
        a.send(kDisconnect);
        CHECK(!a.waitState("starting", 3000).is_null());
        CHECK_MSG(waitUntil([&] { return manager.phase() == app::Phase::Idle && world.startedCount() == before + 1; }, 15000),
                  "a Disconnect sent during the login ends the session right after it started");
        CHECK(world.liveWorkers == 0);
    }
    this_thread::sleep_for(milliseconds(400));
    a.skipToEnd();
    a.send(startCommand("binance", "DIESOON", "1m"));
    {
        const json idle = a.waitFor(
            [](const json& m) { return m.value("state", "") == "idle" && m.contains("error"); }, 8000);
        CHECK_MSG(!idle.is_null(), "a session whose workers all ended on their own returns to idle with an error");
        if (!idle.is_null()) {
            CHECK(idle.value("error", "").find("unexpectedly") != string::npos);
            CHECK(idle.value("market", "") == "binance");
        }
    }
    CHECK(manager.phase() == app::Phase::Idle);

    // ---------------------------------------------------------------------
    section("10. Shutdown with a session running ends it cleanly");
    a.skipToEnd();
    a.send(startCommand("binance", "XRPUSDT", "1m"));
    CHECK(!a.waitState("running", 8000).is_null());
    CHECK(waitUntil([&] { return world.liveWorkers == 1; }, 3000));
    manager.shutdown();
    CHECK(world.liveWorkers == 0);

    viz.stop();
    error_code ec;
    filesystem::remove_all(scratch, ec);
    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}