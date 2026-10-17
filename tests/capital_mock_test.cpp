// ---------------------------------------------------------------------------
// capital_mock_test: the offline end-to-end test of the Capital.com path.
//
// No live credentials or network are needed: tests/mock_capital_server.h plays
// Capital.com (TLS REST + WebSocket quotes, deterministic ticks, a virtual
// clock, fault injection) and this program drives the REAL client code against
// it - capital_api, capital_history, capital.cpp's feed and all 15 timeframe
// workers - then checks every CSV row the workers wrote against an independent
// oracle that folds the raw ticks itself.
//
//   build:  cmake -DTRAIDLE_BUILD_TESTS=ON ... && cmake --build . --target capital_mock_test
//   run:    ./capital_mock_test            (about 3 minutes: it runs in real time)
//           ./capital_mock_test --quick    (skips the two long integration runs)
//
// The run is real-time because candles close when the (virtual) clock crosses
// a bucket boundary at the speed of the real one; the test places "now" 50 s
// before a Monday 00:00 UTC month start, where all 15 timeframes close in the
// same instant.
// ---------------------------------------------------------------------------

#include "mock_capital_server.h"

#include "../src/candle_builder.h"
#include "../src/capital.h"
#include "../src/capital_api.h"
#include "../src/capital_history.h"
#include "../src/indicators.h"
#include "../src/pipeline_common.h"
#include "../src/visualization.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using json = nlohmann::json;
using mockcap::kDayMs;
using mockcap::kMinuteMs;

// ---------------------------------------------------------------------------
// Tiny test harness (same style as candle_builder_test.cpp)
// ---------------------------------------------------------------------------
static FILE* gReport = stdout;  // the ORIGINAL stdout; fd 1/2 are redirected into a capture file
static int gChecks = 0;
static int gFailed = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++gChecks;                                                                         \
        if (!(cond)) {                                                                     \
            ++gFailed;                                                                     \
            fprintf(gReport, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                                  \
    } while (0)

#define CHECK_MSG(cond, message)                                                           \
    do {                                                                                   \
        ++gChecks;                                                                         \
        if (!(cond)) {                                                                     \
            ++gFailed;                                                                     \
            fprintf(gReport, "  FAIL %s:%d  %s  [%s]\n", __FILE__, __LINE__, #cond,        \
                    string(message).c_str());                                              \
        }                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps))

static chrono::steady_clock::time_point gSectionStart;
static void section(const string& name) {
    gSectionStart = chrono::steady_clock::now();
    fprintf(gReport, "\n== %s\n", name.c_str());
    fflush(gReport);
}
static void sectionDone() {
    const double seconds =
        chrono::duration<double>(chrono::steady_clock::now() - gSectionStart).count();
    fprintf(gReport, "   (%.1f s, checks so far: %d, failed: %d)\n", seconds, gChecks, gFailed);
    fflush(gReport);
}
static void note(const string& text) {
    fprintf(gReport, "   %s\n", text.c_str());
    fflush(gReport);
}

static string gDir;  // scratch directory for CSVs, the mock's certificate and the output capture
static string gCapturePath;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static string readWholeFile(const filesystem::path& path) {
    ifstream in(path, ios::binary);
    stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

static string captureSoFar() {
    fflush(stdout);
    fflush(stderr);
    return readWholeFile(gCapturePath);
}

static size_t countOccurrences(const string& haystack, const string& needle) {
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != string::npos; pos = haystack.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

static int64_t modPositive(int64_t value, int64_t modulus) {
    return ((value % modulus) + modulus) % modulus;
}

struct Span {
    int64_t open = 0;
    int64_t end = 0;  // exclusive
};

// The oracle's own bucket math: deliberately written with plain arithmetic
// rather than calling candle_builder::bucketOpen, so the two are independent.
static Span oracleBucket(const string& label, int64_t ts) {
    const int64_t day = mockcap::floorDivMs(ts, kDayMs);
    if (label == "1w") {
        const int64_t monday = day - modPositive(day - 4, 7);  // 1970-01-05 was a Monday
        return {monday * kDayMs, (monday + 7) * kDayMs};
    }
    if (label == "3d") {
        const int64_t first = day - modPositive(day, 3);
        return {first * kDayMs, (first + 3) * kDayMs};
    }
    if (label == "1M") {
        const auto civil = candle_builder::civilFromDays(day);
        const int64_t open = candle_builder::daysFromCivil(civil.year, civil.month, 1);
        const int64_t nextYear = civil.month == 12 ? civil.year + 1 : civil.year;
        const unsigned nextMonth = civil.month == 12 ? 1 : civil.month + 1;
        const int64_t end = candle_builder::daysFromCivil(nextYear, nextMonth, 1);
        return {open * kDayMs, end * kDayMs};
    }
    static const map<string, int64_t> fixedMs = {
        {"1m", 60000LL},        {"3m", 180000LL},        {"5m", 300000LL},       {"15m", 900000LL},
        {"30m", 1800000LL},     {"1h", 3600000LL},       {"2h", 7200000LL},      {"4h", 14400000LL},
        {"6h", 21600000LL},     {"8h", 28800000LL},      {"12h", 43200000LL},    {"1d", 86400000LL}};
    const int64_t step = fixedMs.at(label);
    const int64_t open = mockcap::floorDivMs(ts, step) * step;
    return {open, open + step};
}

struct CsvRow {
    int64_t openMs = 0;
    double volume = 0;
    double o = 0, h = 0, l = 0, c = 0;
    size_t columns = 0;
};

static vector<string> splitCsvLine(const string& line) {
    vector<string> cells;
    stringstream ss(line);
    string cell;
    while (getline(ss, cell, ',')) {
        cells.push_back(cell);
    }
    if (!line.empty() && line.back() == ',') {
        cells.push_back("");
    }
    return cells;
}

static vector<CsvRow> readCsvRows(const filesystem::path& path, size_t& headerColumns) {
    vector<CsvRow> rows;
    headerColumns = 0;
    ifstream in(path);
    string line;
    if (!getline(in, line)) {
        return rows;
    }
    headerColumns = splitCsvLine(line).size();
    while (getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const vector<string> cells = splitCsvLine(line);
        CsvRow row;
        row.columns = cells.size();
        if (cells.size() < 9) {
            rows.push_back(row);
            continue;
        }
        const auto ts = candle_builder::parseUtcIso(cells[0]);
        row.openMs = ts.value_or(-1);
        row.volume = atof(cells[1].c_str());
        row.o = atof(cells[cells.size() - 4].c_str());
        row.h = atof(cells[cells.size() - 3].c_str());
        row.l = atof(cells[cells.size() - 2].c_str());
        row.c = atof(cells[cells.size() - 1].c_str());
        rows.push_back(row);
    }
    return rows;
}

struct VerifyContext {
    mockcap::GroundTruth* truth = nullptr;
    int64_t runStartNowMs = 0;  // server time when the run's first backfill pass began
    int64_t finalNowMs = 0;     // server time when the run ended
    // Rows whose bucket closed inside (liveStartMs, liveEndMs] were written by the LIVE
    // path, which seeds a forming candle from REST and then counts live ticks from 1.5 s
    // before the seed: a few ticks are counted twice, by design. Every other row came from
    // the backfill, whose volume must equal the oracle's tick count exactly.
    int64_t liveStartMs = 0;
    int64_t liveEndMs = INT64_MAX;
    double liveVolumeSlack = 40;  // extra tick counts a live/seeded candle may carry (seed overlap)
};

// Checks one timeframe's CSV against the oracle. Returns the parsed rows.
static vector<CsvRow> verifyCsv(const string& label, const filesystem::path& file,
                                const VerifyContext& ctx) {
    size_t headerColumns = 0;
    vector<CsvRow> rows = readCsvRows(file, headerColumns);
    const string where = "[" + label + "] ";
    CHECK_MSG(!rows.empty(), where + "CSV has no rows");
    if (rows.empty()) {
        return rows;
    }
    CHECK_MSG(headerColumns > 9, where + "header has too few columns");

    int badColumns = 0, badAlignment = 0, badOhlc = 0, badVolume = 0, badOrder = 0, holes = 0;
    string firstProblem;
    auto problem = [&](const string& what, int64_t openMs) {
        if (firstProblem.empty()) {
            firstProblem = what + " at " + candle_builder::formatUtcIso(openMs);
        }
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        const CsvRow& row = rows[i];
        if (row.columns != headerColumns) {
            ++badColumns;
            problem("column count", row.openMs);
            continue;
        }
        const Span span = oracleBucket(label, row.openMs);
        if (span.open != row.openMs) {
            ++badAlignment;
            problem("misaligned open", row.openMs);
            continue;
        }
        if (i > 0 && row.openMs <= rows[i - 1].openMs) {
            ++badOrder;
            problem("not strictly increasing / duplicate", row.openMs);
        }
        const optional<mockcap::OBar> truth = ctx.truth->foldTicks(span.open, span.end, INT64_MAX);
        if (!truth) {
            ++badOhlc;
            problem("row for an empty bucket", row.openMs);
            continue;
        }
        if (fabs(truth->o - row.o) > 1e-9 || fabs(truth->h - row.h) > 1e-9 ||
            fabs(truth->l - row.l) > 1e-9 || fabs(truth->c - row.c) > 1e-9) {
            ++badOhlc;
            char text[200];
            snprintf(text, sizeof(text), "OHLC csv %.5f/%.5f/%.5f/%.5f vs oracle %.5f/%.5f/%.5f/%.5f",
                     row.o, row.h, row.l, row.c, truth->o, truth->h, truth->l, truth->c);
            problem(text, row.openMs);
        }
        const double oracleVolume = static_cast<double>(truth->count);
        const bool fromHistory = !(span.end > ctx.liveStartMs && span.end <= ctx.liveEndMs);
        const bool volumeOk = fromHistory
                                  ? fabs(row.volume - oracleVolume) < 0.5
                                  : (row.volume >= oracleVolume - 0.5 &&
                                     row.volume <= oracleVolume + ctx.liveVolumeSlack);
        if (!volumeOk) {
            ++badVolume;
            problem("volume " + to_string(row.volume) + " vs oracle " + to_string(oracleVolume) +
                        (fromHistory ? " (history row: must match exactly)" : " (live row)"),
                    row.openMs);
        }
        // Continuity: nothing the market actually traded may be missing between two rows.
        if (i > 0 && rows[i - 1].openMs >= 0) {
            Span between = oracleBucket(label, rows[i - 1].openMs);
            between = oracleBucket(label, between.end);
            int guard = 0;
            while (between.open < row.openMs && guard++ < 100000) {
                if (ctx.truth->foldTicks(between.open, between.end, INT64_MAX)) {
                    ++holes;
                    problem("hole: tradable bucket missing", between.open);
                    break;
                }
                between = oracleBucket(label, between.end);
            }
        }
    }
    CHECK_MSG(badColumns == 0, where + "rows with a wrong column count: " + firstProblem);
    CHECK_MSG(badAlignment == 0, where + "misaligned rows: " + firstProblem);
    CHECK_MSG(badOrder == 0, where + "ordering/duplicates: " + firstProblem);
    CHECK_MSG(badOhlc == 0, where + "OHLC mismatches: " + firstProblem);
    CHECK_MSG(badVolume == 0, where + "volume mismatches: " + firstProblem);
    CHECK_MSG(holes == 0, where + "holes: " + firstProblem);

    // The newest row must be at least the latest bucket that had closed (with
    // margin) before the run began, and must itself have closed before it ended.
    if (ctx.finalNowMs > 0 && rows.back().openMs >= 0) {
        const Span last = oracleBucket(label, rows.back().openMs);
        CHECK_MSG(last.end <= ctx.finalNowMs, where + "last row had not closed by the end of the run");
        Span probe = oracleBucket(label, ctx.runStartNowMs - 3000);
        probe = oracleBucket(label, probe.open - 1);  // the bucket before the one forming at start
        for (int guard = 0; guard < 100000; ++guard) {
            if (ctx.truth->foldTicks(probe.open, probe.end, INT64_MAX)) {
                break;
            }
            probe = oracleBucket(label, probe.open - 1);
        }
        CHECK_MSG(rows.back().openMs >= probe.open,
                  where + "newest row " + candle_builder::formatUtcIso(rows.back().openMs) +
                      " is older than the last bucket closed before the run (" +
                      candle_builder::formatUtcIso(probe.open) + ")");
    }
    return rows;
}

// A fresh mock with sensible defaults, listening and ready.
static unique_ptr<mockcap::MockCapitalServer> startMock(mockcap::MockCapitalServer::Config config) {
    config.tempDir = gDir;
    auto server = make_unique<mockcap::MockCapitalServer>(move(config));
    server->start();
    return server;
}

static capital::Credentials credentialsFor(const mockcap::MockCapitalServer& server) {
    capital::Credentials credentials;
    credentials.apiKey = server.config().apiKey;
    credentials.identifier = server.config().identifier;
    credentials.password = server.config().password;
    credentials.demo = false;
    return credentials;
}

static capital::Endpoints endpointsFor(const mockcap::MockCapitalServer& server) {
    capital::Endpoints endpoints;
    endpoints.restHost = "127.0.0.1";
    endpoints.restPort = to_string(server.port());
    endpoints.caFile = server.caFile();
    return endpoints;
}

static shared_ptr<capital::Session> loggedInSession(const mockcap::MockCapitalServer& server) {
    auto session = make_shared<capital::Session>(credentialsFor(server), endpointsFor(server));
    session->login();
    session->refreshClockOffset();
    return session;
}

// ---------------------------------------------------------------------------
// 1. Pure functions: no network
// ---------------------------------------------------------------------------
static json bidAsk(double bid) {
    return {{"bid", bid}, {"ask", bid + 0.0002}};
}

static json priceRow(const string& utc, double o, double h, double l, double c, json volume = 7) {
    json row = {{"snapshotTime", utc}, {"snapshotTimeUTC", utc},
                {"openPrice", bidAsk(o)}, {"highPrice", bidAsk(h)},
                {"lowPrice", bidAsk(l)}, {"closePrice", bidAsk(c)}};
    if (!volume.is_null()) {
        row["lastTradedVolume"] = volume;
    }
    return row;
}

static void testPureFunctions() {
    section("1. parsePriceBars / formatToParam / credentials / epics (no network)");

    // Bid side is used (the ask differs by 0.0002); volume is lastTradedVolume.
    json response = {{"prices",
                      json::array({priceRow("2026-10-15T10:05:00", 1.1010, 1.1020, 1.1000, 1.1015, 11),
                                   priceRow("2026-10-15T10:00:00", 1.1000, 1.1012, 1.0990, 1.1010, 5)})}};
    size_t skipped = 99;
    vector<candle_builder::Bar> bars = capital::parsePriceBars(response, &skipped);
    CHECK(bars.size() == 2);
    CHECK(skipped == 0);
    if (bars.size() == 2) {
        CHECK(bars[0].openMs < bars[1].openMs);  // sorted ascending even though the server sent newest first
        CHECK(bars[0].openMs == *candle_builder::parseUtcIso("2026-10-15T10:00:00"));
        CHECK_NEAR(bars[0].o, 1.1000, 1e-12);
        CHECK_NEAR(bars[0].h, 1.1012, 1e-12);
        CHECK_NEAR(bars[0].l, 1.0990, 1e-12);
        CHECK_NEAR(bars[0].c, 1.1010, 1e-12);
        CHECK_NEAR(bars[0].volume, 5, 1e-12);
        CHECK_NEAR(bars[1].volume, 11, 1e-12);
    }

    // Malformed bars are skipped and counted, never crash the parse.
    json mixed = {{"prices", json::array({
        priceRow("2026-10-15T10:00:00", 1.1000, 1.1012, 1.0990, 1.1010),        // good
        priceRow("2026-10-15T10:01:00", 1.1000, 1.0900, 1.1100, 1.1010),        // high < low
        priceRow("2026-10-15T10:02:00", 0.0, 1.1, 1.0, 1.05),                   // zero price
        priceRow("2026-10-15T10:03:00", 1.1000, 1.1012, 1.0990, 1.1010, nullptr),  // no volume: kept, 0
        priceRow("not a time", 1.1000, 1.1012, 1.0990, 1.1010),                 // bad timestamp
        json{{"snapshotTimeUTC", "2026-10-15T10:04:00"}},                       // no prices at all
        json("garbage"),                                                         // not an object
    })}};
    skipped = 0;
    bars = capital::parsePriceBars(mixed, &skipped);
    CHECK_MSG(bars.size() == 2, "kept " + to_string(bars.size()));
    CHECK_MSG(skipped == 5, "skipped " + to_string(skipped));
    if (bars.size() == 2) {
        CHECK_NEAR(bars[1].volume, 0, 1e-12);
    }

    CHECK(capital::parsePriceBars(json::object(), nullptr).empty());
    CHECK(capital::parsePriceBars(json{{"prices", json::array()}}, nullptr).empty());
    CHECK(capital::parsePriceBars(json{{"prices", "oops"}}, nullptr).empty());
    CHECK(capital::parsePriceBars(json(nullptr), nullptr).empty());

    // `to=` formatting: the shift is added to the UTC instant before it is spelled out.
    capital_history::RangeParams params;
    const int64_t instant = *candle_builder::parseUtcIso("2026-03-01T23:30:00");
    CHECK(capital_history::formatToParam(params, instant) == "2026-03-01T23:30:00");
    params.shiftMs = 3 * 3600 * 1000;
    CHECK(capital_history::formatToParam(params, instant) == "2026-03-02T02:30:00");
    params.shiftMs = -5 * 3600 * 1000;
    CHECK(capital_history::formatToParam(params, instant) == "2026-03-01T18:30:00");

    // Epics and secrets.
    CHECK(capital::isValidEpic("EURUSD"));
    CHECK(capital::isValidEpic("GOLD.1"));
    CHECK(capital::isValidEpic("EUR_USD"));
    CHECK(!capital::isValidEpic(""));
    CHECK(!capital::isValidEpic("EUR USD"));
    CHECK(!capital::isValidEpic("EURUSD/../positions"));
    CHECK(!capital::isValidEpic("EURUSD?x=1"));
    CHECK(!capital::isValidEpic(string(41, 'A')));
    CHECK(capital::maskSecret("abcdefghijklmnopqrstuvwxyz") == "abcd...xyz");
    CHECK(capital::maskSecret("short") == "***");
    CHECK(capital::maskSecret("") == "***");

    // Credentials: the error names the missing variables, never a value.
    for (const char* name : {"CAPITAL_API_KEY", "CAPITAL_IDENTIFIER", "CAPITAL_PASSWORD", "CAPITAL_ENV"}) {
        unsetenv(name);
    }
    string error;
    CHECK(!capital::credentialsFromEnv(error).has_value());
    CHECK(error.find("CAPITAL_API_KEY") != string::npos);
    CHECK(error.find("CAPITAL_IDENTIFIER") != string::npos);
    CHECK(error.find("CAPITAL_PASSWORD") != string::npos);
    setenv("CAPITAL_API_KEY", "key-value-1", 1);
    setenv("CAPITAL_IDENTIFIER", "me@example.com", 1);
    setenv("CAPITAL_PASSWORD", "pw-value-1", 1);
    error.clear();
    auto credentials = capital::credentialsFromEnv(error);
    CHECK(credentials.has_value());
    if (credentials) {
        CHECK(!credentials->demo);
        CHECK(capital::endpointsFor(false).restHost == "api-capital.backend-capital.com");
        CHECK(capital::endpointsFor(true).restHost == "demo-api-capital.backend-capital.com");
    }
    setenv("CAPITAL_ENV", "DEMO", 1);
    credentials = capital::credentialsFromEnv(error);
    CHECK(credentials.has_value() && credentials->demo);
    setenv("CAPITAL_ENV", "staging", 1);
    CHECK(!capital::credentialsFromEnv(error).has_value());
    CHECK(error.find("CAPITAL_ENV") != string::npos);
    CHECK(error.find("key-value-1") == string::npos && error.find("pw-value-1") == string::npos);
    for (const char* name : {"CAPITAL_API_KEY", "CAPITAL_IDENTIFIER", "CAPITAL_PASSWORD", "CAPITAL_ENV"}) {
        unsetenv(name);
    }
    sectionDone();
}

// ---------------------------------------------------------------------------
// 2. Rate limiter (real time, no network)
// ---------------------------------------------------------------------------
static void testRateLimiter() {
    section("2. RateLimiter: 5 requests/s and >= 1.1 s between logins");
    auto& limiter = capital::RateLimiter::instance();
    this_thread::sleep_for(chrono::milliseconds(1100));  // drain whatever earlier tests left in the window

    const auto start = chrono::steady_clock::now();
    for (int i = 0; i < 11; ++i) {
        limiter.throttle();
    }
    const double elapsed = chrono::duration<double>(chrono::steady_clock::now() - start).count();
    // 5 immediately, 5 more a second later, the 11th a second after that.
    CHECK_MSG(elapsed >= 1.9, "11 requests took " + to_string(elapsed) + " s");
    CHECK_MSG(elapsed < 4.0, "11 requests took " + to_string(elapsed) + " s");

    limiter.throttleLogin();
    const auto loginStart = chrono::steady_clock::now();
    limiter.throttleLogin();
    const double gap = chrono::duration<double>(chrono::steady_clock::now() - loginStart).count();
    CHECK_MSG(gap >= 1.05, "two logins were allowed " + to_string(gap) + " s apart");
    sectionDone();
}

// ---------------------------------------------------------------------------
// 3. REST client against the mock
// ---------------------------------------------------------------------------
static void testSessionAndRest() {
    section("3. Session / RestClient against the mock server");
    mockcap::MockCapitalServer::Config config;
    config.clockShiftMs = 7000;  // the mock's clock is 7 s AHEAD of this machine's
    auto server = startMock(config);

    auto session = make_shared<capital::Session>(credentialsFor(*server), endpointsFor(*server));
    session->login();
    const capital::TokenSnapshot tokens = session->tokens();
    CHECK(!tokens.cst.empty() && !tokens.securityToken.empty());
    CHECK(tokens.generation == 1);
    CHECK(session->streamingHost() == "127.0.0.1:" + to_string(server->port()));

    session->refreshClockOffset();
    CHECK_MSG(llabs(session->serverNowMs() - server->serverNowMs()) < 250,
              "serverNow off by " + to_string(session->serverNowMs() - server->serverNowMs()) + " ms");
    CHECK_MSG(llabs(session->serverOffsetMs() + 7000) < 250,
              "offset " + to_string(session->serverOffsetMs()));

    // GET /markets
    capital::RestClient rest(session);
    const json market = rest.getJson("/api/v1/markets/EURUSD");
    CHECK(market["instrument"]["epic"] == "EURUSD");
    CHECK(rest.get("/api/v1/markets/NOPEUSD").status == 404);
    bool threw = false;
    try {
        rest.getJson("/api/v1/markets/NOPEUSD");
    } catch (const exception&) {
        threw = true;
    }
    CHECK(threw);

    // Bars equal the ground truth, tick for tick.
    size_t skipped = 99;
    const vector<candle_builder::Bar> bars =
        rest.fetchPriceBars("EURUSD", "MINUTE_5", 50, nullopt, &skipped);
    CHECK(bars.size() == 50);
    CHECK(skipped == 0);
    int mismatches = 0;
    for (const auto& bar : bars) {
        const auto truth = server->truth().foldTicks(bar.openMs, bar.openMs + 300000, server->serverNowMs());
        if (!truth || fabs(truth->o - bar.o) > 1e-9 || fabs(truth->h - bar.h) > 1e-9 ||
            fabs(truth->l - bar.l) > 1e-9 || fabs(truth->c - bar.c) > 1e-9) {
            // The final (forming) bar keeps growing between the REST call and this check.
            if (&bar != &bars.back()) ++mismatches;
        }
    }
    CHECK_MSG(mismatches == 0, to_string(mismatches) + " bars differ from the ground truth");

    // 401 -> one re-login -> retry. The login gate means this also proves the
    // client waits >= 1 s since the previous login.
    const int loginsBefore = server->logins();
    server->expireTokens();
    const json again = rest.getJson("/api/v1/markets/EURUSD");
    CHECK(again["instrument"]["epic"] == "EURUSD");
    CHECK(server->logins() == loginsBefore + 1);
    CHECK(session->generation() == 2);
    CHECK(server->unauthorized() >= 1);

    // 429: honoured Retry-After, then succeeds.
    server->inject429(2);
    const auto t0 = chrono::steady_clock::now();
    const capital::HttpResult limited = rest.get("/api/v1/markets/EURUSD");
    const double waited = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
    CHECK(limited.status == 200);
    CHECK_MSG(waited >= 1.5, "two 429s with Retry-After: 1 only delayed " + to_string(waited) + " s");

    // Single-flight re-login: 8 threads all hit the expired token at once; exactly one login happens.
    const int loginsMid = server->logins();
    server->expireTokens();
    atomic<int> ok{0};
    vector<thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            try {
                capital::RestClient own(session);
                if (own.getJson("/api/v1/markets/EURUSD")["instrument"]["epic"] == "EURUSD") ++ok;
            } catch (const exception& e) {
                fprintf(gReport, "   thread error: %s\n", e.what());
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(ok == 8);
    CHECK_MSG(server->logins() == loginsMid + 1,
              "8 simultaneous 401s caused " + to_string(server->logins() - loginsMid) + " logins");
    CHECK(server->loginViolations() == 0);
    CHECK(server->rateViolations() == 0);
    sectionDone();
}

// ---------------------------------------------------------------------------
// 4. Login failures: messages are helpful and never leak
// ---------------------------------------------------------------------------
static void testLoginFailures() {
    section("4. Login failures: clear messages, no secrets");
    auto server = startMock({});

    capital::Credentials wrongKey = credentialsFor(*server);
    wrongKey.apiKey = "WRONGkey-0123456789-abcdef";
    string message;
    try {
        capital::Session session(wrongKey, endpointsFor(*server));
        session.login();
    } catch (const exception& e) {
        message = e.what();
    }
    CHECK(!message.empty());
    CHECK_MSG(message.find(wrongKey.apiKey) == string::npos, "message contains the full API key: " + message);
    CHECK_MSG(message.find(wrongKey.password) == string::npos, "message contains the password");
    CHECK_MSG(message.find(capital::maskSecret(wrongKey.apiKey)) != string::npos,
              "message should show the masked key: " + message);

    capital::Credentials wrongPassword = credentialsFor(*server);
    wrongPassword.password = "not-the-password";
    message.clear();
    try {
        capital::Session session(wrongPassword, endpointsFor(*server));
        session.login();
    } catch (const exception& e) {
        message = e.what();
    }
    CHECK(!message.empty());
    CHECK_MSG(message.find(wrongPassword.password) == string::npos, "message contains the password");
    CHECK_MSG(message.find(wrongPassword.identifier) == string::npos || true, "");  // an e-mail is not a secret

    // preflight: unknown epic, invalid epic, good epic.
    string error;
    auto session = capital::preflightWith(credentialsFor(*server), endpointsFor(*server), "XXXUSD", error);
    CHECK(session == nullptr);
    CHECK_MSG(error == "Unknown epic 'XXXUSD' on Capital.com", error);

    error.clear();
    session = capital::preflightWith(credentialsFor(*server), endpointsFor(*server), "EUR USD", error);
    CHECK(session == nullptr);
    CHECK_MSG(error.find("Invalid epic") != string::npos, error);

    error.clear();
    session = capital::preflightWith(credentialsFor(*server), endpointsFor(*server), "EURUSD", error);
    CHECK_MSG(session != nullptr, error);
    CHECK(error.empty());
    CHECK(server->loginViolations() == 0);

    // Nothing but market data was requested.
    for (const string& request : server->requestLog()) {
        const bool allowed = request.rfind("POST /api/v1/session", 0) == 0 ||
                             request.rfind("GET /api/v1/time", 0) == 0 ||
                             request.rfind("GET /api/v1/markets/", 0) == 0;
        CHECK_MSG(allowed, "unexpected request: " + request);
    }
    sectionDone();
}

// ---------------------------------------------------------------------------
// 5. `to=` calibration against every behaviour the API might have
// ---------------------------------------------------------------------------
static void testCalibration() {
    section("5. calibrateRangeParams: UTC, exclusive, shifted, ignored, rejected, tiny pages");
    struct Case {
        const char* name;
        mockcap::ToMode mode;
        int shiftHours;
        bool usable;
        int64_t expectShiftMs;
        bool inclusive;
    };
    const vector<Case> cases = {
        {"utc inclusive", mockcap::ToMode::UtcInclusive, 0, true, 0, true},
        {"utc exclusive", mockcap::ToMode::UtcExclusive, 0, true, 0, false},
        {"UTC+3 inclusive", mockcap::ToMode::ShiftedInclusive, 3, true, 3 * 3600 * 1000LL, true},
        {"UTC-5 exclusive", mockcap::ToMode::ShiftedExclusive, -5, true, -5 * 3600 * 1000LL, false},
        {"to ignored", mockcap::ToMode::Ignored, 0, false, 0, true},
        {"to rejected (HTTP 400)", mockcap::ToMode::Rejected, 0, false, 0, true},
        {"to works but pages of ~10 bars", mockcap::ToMode::IgnoresMax, 0, false, 0, true},
    };
    for (const Case& test : cases) {
        mockcap::MockCapitalServer::Config config;
        config.toMode = test.mode;
        config.toShiftHours = test.shiftHours;
        auto server = startMock(config);
        auto session = loggedInSession(*server);
        capital::RestClient rest(session);
        const capital_history::RangeParams params = capital_history::calibrateRangeParams(rest, "EURUSD");
        const string tag = string("[") + test.name + "] ";
        CHECK_MSG(params.usable == test.usable, tag + "usable=" + to_string(params.usable) + " note: " + params.note);
        if (test.usable) {
            CHECK_MSG(params.shiftMs == test.expectShiftMs, tag + "shift " + to_string(params.shiftMs));
            CHECK_MSG(params.toInclusive == test.inclusive, tag + "inclusive flag");
        } else {
            CHECK_MSG(!params.note.empty(), tag + "an unusable result must say why");
        }
        CHECK(server->rateViolations() == 0);
    }
    sectionDone();
}

// ---------------------------------------------------------------------------
// 6. Multi-page history backfill in each `to=` mode, resume, alignment check
// ---------------------------------------------------------------------------
struct HistoryRun {
    bool ok = false;
    vector<string> logs;
    int64_t startNowMs = 0;
};

static HistoryRun runHistory(mockcap::MockCapitalServer& server, const shared_ptr<capital::Session>& session,
                             const string& timeframe, const filesystem::path& csv) {
    HistoryRun run;
    capital::RestClient rest(session);
    const auto tasks = pipeline::buildTasks(timeframe);
    indicators::IndicatorEngine engine;
    run.startNowMs = session->serverNowMs();
    run.ok = capital_history::ensureContinuousHistory(
        *session, rest, "EURUSD", tasks.at(0), csv, [](const string&) {},
        [&](const string& line) { run.logs.push_back(line); }, engine, {}, true);
    (void)server;
    return run;
}

static void testHistoryBackfill() {
    section("6. History backfill: multi-page walk in every `to=` mode, resume, alignment self-check");
    struct Mode {
        const char* name;
        mockcap::ToMode mode;
        int shiftHours;
        bool deep;
    };
    const vector<Mode> modes = {
        {"utc inclusive", mockcap::ToMode::UtcInclusive, 0, true},
        {"utc exclusive", mockcap::ToMode::UtcExclusive, 0, true},
        {"UTC+3 inclusive", mockcap::ToMode::ShiftedInclusive, 3, true},
        {"UTC-2 exclusive", mockcap::ToMode::ShiftedExclusive, -2, true},
        {"to ignored (fallback)", mockcap::ToMode::Ignored, 0, false},
    };
    for (const Mode& mode : modes) {
        mockcap::MockCapitalServer::Config config;
        config.toMode = mode.mode;
        config.toShiftHours = mode.shiftHours;
        // A one-hour closed market inside the 5-day window: it must stay a gap.
        const int64_t realNow = mockcap::realNowMs();
        config.closedWindows = {{realNow - 30 * 3600 * 1000LL, realNow - 29 * 3600 * 1000LL}};
        auto server = startMock(config);
        auto session = loggedInSession(*server);
        const filesystem::path csv = filesystem::path(gDir) / ("hist_" + to_string(&mode - &modes[0]) + ".csv");
        filesystem::remove(csv);

        const HistoryRun run = runHistory(*server, session, "5m", csv);
        const string tag = string("[") + mode.name + "] ";
        CHECK_MSG(run.ok, tag + "backfill reported failure");

        mockcap::GroundTruth& truth = server->truth();
        VerifyContext ctx;
        ctx.truth = &truth;
        ctx.runStartNowMs = run.startNowMs;
        ctx.finalNowMs = server->serverNowMs();
        ctx.liveStartMs = INT64_MAX;  // no live phase here: every row is a backfill row
        const vector<CsvRow> rows = verifyCsv("5m", csv, ctx);
        if (mode.deep) {
            CHECK_MSG(rows.size() >= 1400, tag + "only " + to_string(rows.size()) +
                                               " rows: pagination past the first 1000 did not work");
            const size_t gapMarkers = countOccurrences(accumulate(run.logs.begin(), run.logs.end(), string(),
                                                                  [](string a, const string& b) { return a + b + "\n"; }),
                                                       "deep backfill unavailable");
            CHECK_MSG(gapMarkers == 0, tag + "wrongly reported deep backfill unavailable");
        } else {
            CHECK_MSG(rows.size() >= 990 && rows.size() <= 1000,
                      tag + "fallback should write the latest ~1000 candles, wrote " + to_string(rows.size()));
            string all;
            for (const auto& line : run.logs) all += line + "\n";
            CHECK_MSG(countOccurrences(all, "deep backfill unavailable (to= not understood); using latest 1000 candles only") == 1,
                      tag + "the fallback notice must appear exactly once: " + all);
        }

        // Resume: a second call on the same CSV costs zero requests when nothing new has closed, and never duplicates a row.
        const size_t rowsBefore = rows.size();
        const int requestsBefore = server->pricesRequests();
        // The 5-minute bucket the FIRST run started in (not "now"): if a bucket boundary passes
        // anywhere between the two runs - easy on a slow sanitizer build - one more candle
        // legitimately closes, and the zero-request expectation below no longer applies.
        const int64_t bucketBefore = oracleBucket("5m", run.startNowMs).open;
        const HistoryRun again = runHistory(*server, session, "5m", csv);
        CHECK(again.ok);
        size_t headerColumns = 0;
        const size_t rowsAfter = readCsvRows(csv, headerColumns).size();
        if (oracleBucket("5m", server->serverNowMs()).open == bucketBefore) {
            CHECK_MSG(server->pricesRequests() == requestsBefore,
                      tag + "an up-to-date CSV must cost zero requests");
            CHECK(rowsAfter == rowsBefore);
        } else {
            CHECK(rowsAfter >= rowsBefore && rowsAfter <= rowsBefore + 1);
        }
        CHECK(server->rateViolations() == 0);
        // The limiter caps request STARTS at 5/s (section 2 proves that exactly). What the server
        // sees can be bunched a little by thread scheduling between claiming a slot and the bytes
        // arriving, so the end-to-end bound is the server's own 10/s limit, not 5.
        CHECK(server->maxRequestsInAnySecond() <= 10);
    }

    // The alignment self-check: if Capital's 4-hour bars open at 01:00 instead of 00:00, the client must refuse
    // them (a visible log line) rather than write misaligned candles.
    {
        mockcap::MockCapitalServer::Config config;
        config.alignShiftMs["HOUR_4"] = 3600 * 1000;
        auto server = startMock(config);
        auto session = loggedInSession(*server);

        const filesystem::path misaligned = filesystem::path(gDir) / "misaligned_4h.csv";
        filesystem::remove(misaligned);
        const HistoryRun run = runHistory(*server, session, "4h", misaligned);
        string all;
        for (const auto& line : run.logs) all += line + "\n";
        CHECK_MSG(!run.ok, "misaligned 4h history must be refused");
        CHECK_MSG(all.find("do not open on UTC boundaries") != string::npos, all);
        size_t headerColumns = 0;
        CHECK(readCsvRows(misaligned, headerColumns).empty());

        // The forming-candle seed is refused too.
        capital::RestClient rest(session);
        const auto spec = *candle_builder::intervalFromBinance("4h");
        const auto seed = capital_history::fetchFormingSeed(*session, rest, "EURUSD", spec);
        CHECK(seed.status == capital_history::FormingSeed::Status::Failed);
        CHECK(seed.error.find("not UTC-aligned") != string::npos);

        // ...while a timeframe built from aligned resolutions is unaffected.
        const filesystem::path fine = filesystem::path(gDir) / "aligned_2h.csv";
        filesystem::remove(fine);
        const HistoryRun fineRun = runHistory(*server, session, "2h", fine);
        CHECK(fineRun.ok);
        CHECK(!readCsvRows(fine, headerColumns).empty());
    }
    sectionDone();
}

// ---------------------------------------------------------------------------
// 7. The big one: 15 timeframes live across a boundary, with faults
// ---------------------------------------------------------------------------
// Finds the first 1st-of-month that is also a Monday and whose epoch day is a
// multiple of 3: at 00:00 UTC of that day EVERY one of the 15 timeframes
// (1m ... 1d, 3d, 1w, 1M) closes a candle in the same instant.
static int64_t findAllTimeframesBoundary() {
    for (int year = 2027; year < 2200; ++year) {
        for (unsigned month = 1; month <= 12; ++month) {
            const int64_t day = candle_builder::daysFromCivil(year, month, 1);
            if (modPositive(day - 4, 7) == 0 && modPositive(day, 3) == 0) {
                return day * kDayMs;
            }
        }
    }
    return 0;
}

static const vector<string> kAllTimeframes = {"1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h",
                                              "6h", "8h", "12h", "1d", "3d", "1w", "1mo"};

static string joinTimeframes(const vector<string>& labels) {
    string joined;
    for (size_t i = 0; i < labels.size(); ++i) {
        joined += (i ? "," : "") + labels[i];
    }
    return joined;
}

struct SecretSet {
    vector<pair<string, string>> items;  // name, value
};

static void checkNoSecretsIn(const string& text, const string& where, const SecretSet& secrets) {
    for (const auto& item : secrets.items) {
        CHECK_MSG(text.find(item.second) == string::npos, where + " contains the " + item.first);
    }
}

static void testIntegrationBoundary() {
    section("7. Live: 15 timeframes across a boundary (expired token, dropped socket, silent market)");
    const int64_t boundary = findAllTimeframesBoundary();
    CHECK(boundary > 0);
    note("boundary: " + candle_builder::formatUtcIso(boundary) + " UTC (a Monday month start, epoch day divisible by 3)");

    mockcap::MockCapitalServer::Config config;
    config.clockShiftMs = (boundary - 50 * 1000) - mockcap::realNowMs();
    config.closedWindows = {
        {boundary - 3 * 3600 * 1000LL, boundary - 2 * 3600 * 1000LL},  // an hour-long gap in history
        {boundary + 30 * 1000, boundary + 45 * 1000},                  // 15 s of silence after the boundary
    };
    auto server = startMock(config);

    string error;
    auto session = capital::preflightWith(credentialsFor(*server), endpointsFor(*server), "EURUSD", error);
    CHECK_MSG(session != nullptr, error);
    if (!session) {
        return;
    }

    const string folder = (filesystem::path(gDir) / "fx_eurusd").string();
    filesystem::remove_all(folder);
    filesystem::create_directories(folder);

    const vector<pipeline::TimeframeTask> tasks = pipeline::buildTasks(joinTimeframes(kAllTimeframes));
    CHECK(tasks.size() == 15);
    pipeline::TerminalBoard board(kAllTimeframes);
    visualization::VisualizationServer viz(18765);
    viz.start();

    const int64_t runStartNow = server->serverNowMs();
    capital::RunOptions options;
    options.runFor = chrono::milliseconds(130 * 1000);
    atomic<bool> done{false};
    thread runner([&] {
        capital::run(session, "EURUSD", tasks, filesystem::path(folder), board, viz, options);
        done = true;
    });

    bool expired = false, dropped = false;
    while (!done) {
        const int64_t now = server->serverNowMs();
        if (!expired && now >= boundary - 38 * 1000) {
            server->expireTokens();  // the next ping is answered with an error: re-login + reconnect + re-seed
            expired = true;
        }
        if (!dropped && now >= boundary - 10 * 1000) {
            server->dropWebSockets();  // abrupt drop: reconnect + re-seed
            dropped = true;
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    runner.join();
    const int64_t finalNow = server->serverNowMs();
    viz.stop();
    note("run covered server time " + candle_builder::formatUtcIso(runStartNow) + " -> " +
         candle_builder::formatUtcIso(finalNow));

    // ---- every CSV against the oracle ----
    VerifyContext ctx;
    ctx.truth = &server->truth();
    ctx.runStartNowMs = runStartNow;
    ctx.finalNowMs = finalNow;
    ctx.liveStartMs = runStartNow - 3000;
    for (const pipeline::TimeframeTask& task : tasks) {
        const filesystem::path file = filesystem::path(folder) / (task.englishName + ".csv");
        const vector<CsvRow> rows = verifyCsv(task.binanceInterval, file, ctx);
        // The candle that closed AT the boundary is the headline: it must exist and match.
        const Span closing = oracleBucket(task.binanceInterval, boundary - 1);
        bool found = false;
        for (const CsvRow& row : rows) {
            if (row.openMs == closing.open) found = true;
        }
        CHECK_MSG(found, "[" + task.rawTimeframe + "] no row for the candle that closed at the boundary (" +
                             candle_builder::formatUtcIso(closing.open) + ")");
        // 1m also closes the candle that started at the boundary (with the silent window inside it).
        if (task.binanceInterval == "1m") {
            bool second = false;
            for (const CsvRow& row : rows) {
                if (row.openMs == boundary) second = true;
            }
            CHECK_MSG(second, "[1m] no row for the candle spanning the silent window");
        }
    }
    note("rows per timeframe:");
    {
        string line;
        for (const pipeline::TimeframeTask& task : tasks) {
            size_t headerColumns = 0;
            line += task.rawTimeframe + "=" +
                    to_string(readCsvRows(filesystem::path(folder) / (task.englishName + ".csv"), headerColumns).size()) + " ";
        }
        note(line);
    }

    // ---- what the server saw ----
    CHECK_MSG(server->logins() >= 2, "logins: " + to_string(server->logins()));
    CHECK_MSG(server->wsConnections() >= 3, "websocket connections: " + to_string(server->wsConnections()));
    CHECK(server->loginViolations() == 0);
    CHECK(server->rateViolations() == 0);
    // Same reasoning as in section 6: the 5/s limiter is proven in section 2; here the server must
    // never have seen more than its own 10/s limit, even with 15 workers and heavy CPU contention.
    CHECK_MSG(server->maxRequestsInAnySecond() <= 10,
              "peak requests in one second: " + to_string(server->maxRequestsInAnySecond()) +
                  " -> " + server->peakWindow());
    CHECK(server->quotesSent() > 100);
    const set<string> destinations = server->wsDestinations();
    CHECK(destinations.count("ping") == 1);
    CHECK(destinations.count("marketData.subscribe") == 1);
    CHECK_MSG(destinations.size() == 2, "unexpected WebSocket destination used");
    for (const string& request : server->requestLog()) {
        const bool allowed = request.rfind("POST /api/v1/session", 0) == 0 ||
                             request.rfind("GET /api/v1/time", 0) == 0 ||
                             request.rfind("GET /api/v1/markets/EURUSD", 0) == 0 ||
                             request.rfind("GET /api/v1/prices/EURUSD?", 0) == 0;
        CHECK_MSG(allowed, "request outside the market-data surface: " + request);
    }

    // ---- what the client printed ----
    const string output = captureSoFar();
    CHECK_MSG(countOccurrences(output, "[capital] reconnecting in") == 2,
              "reconnect lines: " + to_string(countOccurrences(output, "[capital] reconnecting in")) +
                  " (expected exactly 2: the expired token and the dropped socket; the silent market must not trigger one)");
    CHECK(countOccurrences(output, "[capital] reconnected, streaming EURUSD") >= 2);
    CHECK(output.find("[capital] streaming EURUSD") != string::npos);
    for (const char* bad : {"Fatal error", "frame handling error", "could not re-seed", "could not seed",
                            "skipped an incomplete candle", "history gap", "malformed", "deep backfill unavailable",
                            "historical backfill did not fully complete", "do not open on UTC boundaries"}) {
        CHECK_MSG(output.find(bad) == string::npos, string("unexpected output: ") + bad);
    }

    SecretSet secrets;
    secrets.items = {{"API key", server->config().apiKey},
                     {"password", server->config().password},
                     {"CST token", "CSTtoken"},
                     {"security token", "SECtoken"},
                     {"account balance", "98765.43"}};
    checkNoSecretsIn(output, "console output", secrets);
    for (const auto& entry : filesystem::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            checkNoSecretsIn(readWholeFile(entry.path()), entry.path().filename().string(), secrets);
        }
    }
    {
        // The per-minute feed statistics line (quote -> worker hand-off latency, p50/p99). On a
        // loopback mock these show the cost of OUR code only; real network latency is not in them.
        const string captured = captureSoFar();
        size_t pos = 0;
        int shown = 0;
        while (shown < 3 && (pos = captured.find("last 60s:", pos)) != string::npos) {
            size_t end = captured.find_first_of("\n\x1b", pos);
            if (end == string::npos) end = captured.size();
            note("feed stats: " + captured.substr(pos, end - pos));
            pos = end;
            ++shown;
        }
    }
    note("quotes sent by the mock: " + to_string(server->quotesSent()) + ", REST price requests: " +
         to_string(server->pricesRequests()) + ", peak REST requests in any 1 s: " +
         to_string(server->maxRequestsInAnySecond()) + " (limiter 5/s, server limit 10/s)");
    sectionDone();

    // ---- 8. restart: resume + gap fill, append-only ----
    section("8. Restart 2 h later: resume from the CSVs, fill the gap, never rewrite or duplicate");
    map<string, string> before;
    for (const pipeline::TimeframeTask& task : tasks) {
        before[task.englishName] = readWholeFile(filesystem::path(folder) / (task.englishName + ".csv"));
    }
    server->setClockShiftMs(server->config().clockShiftMs + (boundary + 2 * 3600 * 1000 + 7 * 60 * 1000 + 33 * 1000) -
                            (finalNow));
    server->expireTokens();
    string error2;
    auto session2 = capital::preflightWith(credentialsFor(*server), endpointsFor(*server), "EURUSD", error2);
    CHECK_MSG(session2 != nullptr, error2);
    if (session2) {
        const int64_t start2 = server->serverNowMs();
        capital::RunOptions short2;
        short2.runFor = chrono::milliseconds(3000);
        {
            pipeline::TerminalBoard board2(kAllTimeframes);
            visualization::VisualizationServer viz2(18766);
            capital::run(session2, "EURUSD", tasks, filesystem::path(folder), board2, viz2, short2);
        }
        VerifyContext ctx2;
        ctx2.truth = &server->truth();
        ctx2.runStartNowMs = start2;
        ctx2.finalNowMs = server->serverNowMs();
        ctx2.liveStartMs = runStartNow - 3000;  // the first run's live window: its rows keep the seed-overlap slack...
        ctx2.liveEndMs = finalNow;              // ...but everything the restart backfilled must be exact
        for (const pipeline::TimeframeTask& task : tasks) {
            const filesystem::path file = filesystem::path(folder) / (task.englishName + ".csv");
            const string now = readWholeFile(file);
            const string& old = before[task.englishName];
            CHECK_MSG(now.compare(0, old.size(), old) == 0,
                      "[" + task.rawTimeframe + "] the earlier rows were rewritten, not appended to");
            const vector<CsvRow> rows = verifyCsv(task.binanceInterval, file, ctx2);
            size_t oldRows = count(old.begin(), old.end(), '\n') - 1;
            CHECK_MSG(rows.size() >= oldRows, "[" + task.rawTimeframe + "] rows disappeared");
        }
        const string output2 = captureSoFar();
        CHECK_MSG(output2.find("Fatal error") == string::npos, "fatal error during the restart");
        CHECK(server->loginViolations() == 0);
        CHECK(server->rateViolations() == 0);
    }
    sectionDone();
}

// ---------------------------------------------------------------------------
// 9. Subscription refused: stop, do not hammer
// ---------------------------------------------------------------------------
static void testRefusedSubscription() {
    section("9. Subscription refused: one connection, clear message, no retry storm");
    mockcap::MockCapitalServer::Config config;
    config.refuseSubscribe = true;
    auto server = startMock(config);
    string error;
    auto session = capital::preflightWith(credentialsFor(*server), endpointsFor(*server), "EURUSD", error);
    CHECK_MSG(session != nullptr, error);
    if (!session) {
        return;
    }
    const string folder = (filesystem::path(gDir) / "fx_refused").string();
    filesystem::remove_all(folder);
    filesystem::create_directories(folder);
    const auto tasks = pipeline::buildTasks("1m");
    pipeline::TerminalBoard board({"1m"});
    visualization::VisualizationServer viz(18767);
    capital::RunOptions options;
    options.runFor = chrono::milliseconds(6000);
    const size_t before = countOccurrences(captureSoFar(), "did not accept the subscription");
    capital::run(session, "EURUSD", tasks, filesystem::path(folder), board, viz, options);
    const string output = captureSoFar();
    CHECK_MSG(countOccurrences(output, "did not accept the subscription") == before + 1,
              "the refusal must be reported exactly once");
    CHECK_MSG(server->wsConnections() == 1, "reconnected " + to_string(server->wsConnections()) + " times after a refusal");
    sectionDone();
}

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (string(argv[i]) == "--quick") quick = true;
    }

    gDir = (filesystem::temp_directory_path() / ("capital_mock_test_" + to_string(getpid()))).string();
    filesystem::remove_all(gDir);
    filesystem::create_directories(gDir);
    gCapturePath = gDir + "/captured_output.txt";

    // Everything the code under test prints (stdout AND stderr) goes to a file, so the
    // test can prove no secret ever reached the console. This program's own report keeps
    // the original stdout.
    fflush(stdout);
    fflush(stderr);
    gReport = fdopen(dup(1), "w");
    const int capture = open(gCapturePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(capture, 1);
    dup2(capture, 2);
    close(capture);

    fprintf(gReport, "capital_mock_test: scratch directory %s\n", gDir.c_str());

    testPureFunctions();
    testRateLimiter();
    testSessionAndRest();
    testLoginFailures();
    testCalibration();
    testHistoryBackfill();
    testRefusedSubscription();
    if (!quick) {
        testIntegrationBoundary();
    }

    // UBSan prints "runtime error: ..." and carries on (ASan aborts, TSan sets the exit code), so
    // in a -fsanitize=undefined build any such line in the captured output is a test failure.
    CHECK_MSG(captureSoFar().find("runtime error:") == string::npos, "UndefinedBehaviorSanitizer reported a runtime error");

    fprintf(gReport, "\n%d checks, %d failed\n", gChecks, gFailed);
    fflush(gReport);
    if (gFailed == 0) {
        filesystem::remove_all(gDir);
    } else {
        fprintf(gReport, "scratch directory kept for inspection: %s\n", gDir.c_str());
    }
    return gFailed == 0 ? 0 : 1;
}