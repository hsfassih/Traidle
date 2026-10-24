#include "../src/utils/csv_history.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

namespace {

int gChecks = 0;
int gFailures = 0;

void report(bool ok, const char* expression, int line) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(condition) report(static_cast<bool>(condition), #condition, __LINE__)

string csvRow(const string& timestamp, const string& baseVolume, const string& open,
              const string& high, const string& low, const string& close) {
    vector<string> fields(23);
    fields[0] = timestamp;
    fields[1] = baseVolume;
    fields[2] = "10";
    fields[3] = "4";
    fields[4] = "5";
    fields[19] = open;
    fields[20] = high;
    fields[21] = low;
    fields[22] = close;

    string row;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            row += ',';
        }
        row += fields[i];
    }
    return row;
}

}  // namespace

int main() {
    const auto unique = chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        filesystem::temp_directory_path() / ("traidle_csv_history_test_" + to_string(unique) + ".csv");

    constexpr int64_t firstOpenMs = 1709251200000LL;
    constexpr int64_t secondOpenMs = firstOpenMs + 60000;

    {
        ofstream csv(path);
        csv << "timestamp,base_volume,quote_volume,taker_buy_base_volume,taker_buy_quote_volume,"
               "indicators,open,high,low,close\n";
        csv << csvRow("2024-03-01 00:00:00", "2", "100", "110", "90", "105") << '\n';
        csv << csvRow("2024-03-01 00:01:00", "3", "105", "115", "100", "112") << '\n';
        csv << csvRow("2024-03-01 00:02:00", "invalid", "112", "120", "110", "118") << '\n';
        csv << "2024-03-01 00:03:00,4,partial";
    }

    const auto lastStored = csv_history::lastStoredCsvOpenTimeMs(path);
    CHECK(lastStored.has_value());
    if (lastStored) {
        CHECK(*lastStored == secondOpenMs);
    }

    vector<candlesticks::Candlestick> replayed;
    vector<indicators::IndicatorSnapshot> snapshots;
    indicators::IndicatorEngine engine;
    csv_history::replayCsvIntoEngine(
        path, engine,
        [&](const candlesticks::Candlestick& candle, const indicators::IndicatorSnapshot& snapshot) {
            replayed.push_back(candle);
            snapshots.push_back(snapshot);
        });

    CHECK(replayed.size() == 2);
    CHECK(snapshots.size() == 2);
    if (replayed.size() == 2) {
        CHECK(replayed[0].openTime == firstOpenMs);
        CHECK(replayed[1].openTime == secondOpenMs);
        CHECK(replayed[0].baseVolume == 2.0);
        CHECK(replayed[1].baseVolume == 3.0);
        CHECK(replayed[0].open == 100.0);
        CHECK(replayed[0].high == 110.0);
        CHECK(replayed[0].low == 90.0);
        CHECK(replayed[0].close == 105.0);
        CHECK(replayed[0].closed);
        CHECK(replayed[1].closed);
    }

    atomic<bool> cancel{true};
    size_t cancelledCallbacks = 0;
    indicators::IndicatorEngine cancelledEngine;
    csv_history::replayCsvIntoEngine(
        path, cancelledEngine,
        [&](const candlesticks::Candlestick&, const indicators::IndicatorSnapshot&) {
            ++cancelledCallbacks;
        },
        &cancel);
    CHECK(cancelledCallbacks == 0);

    filesystem::remove(path);
    fprintf(stderr, "csv_history_test: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
