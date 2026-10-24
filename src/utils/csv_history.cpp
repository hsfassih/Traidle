#include "csv_history.h"

#include <algorithm>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

namespace csv_history {
namespace {

constexpr size_t kTotalColumns = 23;
constexpr size_t kBaseVolumeColumn = 1;
constexpr size_t kFirstIndicatorColumn = 5;
constexpr size_t kLastIndicatorColumn = 18;
constexpr size_t kOpenColumn = 19;
constexpr size_t kHighColumn = 20;
constexpr size_t kLowColumn = 21;
constexpr size_t kCloseColumn = 22;

optional<int64_t> parseUtcTimestampToEpochMs(const string& field) {
    tm parsed{};
    istringstream input(field);
    input >> get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) {
        return nullopt;
    }
#if defined(_WIN32)
    const time_t seconds = _mkgmtime(&parsed);
#else
    const time_t seconds = timegm(&parsed);
#endif
    if (seconds == static_cast<time_t>(-1)) {
        return nullopt;
    }
    return static_cast<int64_t>(seconds) * 1000;
}

vector<string> splitCsvLine(const string& line) {
    vector<string> fields;
    stringstream fieldStream(line);
    string field;
    while (getline(fieldStream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

}  // namespace

optional<int64_t> lastStoredCsvOpenTimeMs(const filesystem::path& csvPath) {
    if (!filesystem::exists(csvPath)) {
        return nullopt;
    }
    ifstream file(csvPath, ios::binary);
    if (!file.is_open()) {
        return nullopt;
    }
    file.seekg(0, ios::end);
    const streamoff size = file.tellg();
    if (size <= 0) {
        return nullopt;
    }
    constexpr streamoff kTailBytes = 8192;
    const streamoff tail = min(size, kTailBytes);
    file.seekg(size - tail);
    string buffer(static_cast<size_t>(tail), '\0');
    file.read(buffer.data(), tail);

    const bool endsWithNewline = !buffer.empty() && buffer.back() == '\n';
    vector<string> lines;
    stringstream stream(buffer);
    string line;
    while (getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (!lines.empty() && !endsWithNewline) {
        lines.pop_back();
    }

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->rfind("timestamp,", 0) == 0 || it->empty()) {
            continue;
        }

        const vector<string> fields = splitCsvLine(*it);
        if (fields.size() != kTotalColumns) {
            continue;
        }

        bool fieldsValid = true;
        for (size_t i = 1; i < fields.size(); ++i) {
            const bool isIndicatorColumn = i >= kFirstIndicatorColumn && i <= kLastIndicatorColumn;
            if (isIndicatorColumn && fields[i].empty()) {
                continue;
            }
            try {
                size_t consumed = 0;
                stod(fields[i], &consumed);
                fieldsValid = fieldsValid && consumed == fields[i].size();
            } catch (const exception&) {
                fieldsValid = false;
            }
            if (!fieldsValid) {
                break;
            }
        }
        if (!fieldsValid) {
            continue;
        }

        if (auto openTimeMs = parseUtcTimestampToEpochMs(fields[0])) {
            return openTimeMs;
        }
    }
    return nullopt;
}

void replayCsvIntoEngine(const filesystem::path& csvPath, indicators::IndicatorEngine& engine,
                         const HistorySeedHandler& seedHistory, const atomic<bool>* cancel) {
    if (!filesystem::exists(csvPath)) {
        return;
    }
    ifstream file(csvPath);
    if (!file.is_open()) {
        return;
    }

    string line;
    bool isHeaderLine = true;
    while (getline(file, line)) {
        if (cancel != nullptr && cancel->load(memory_order_relaxed)) {
            return;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (isHeaderLine) {
            isHeaderLine = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }

        const vector<string> fields = splitCsvLine(line);
        if (fields.size() != kTotalColumns) {
            continue;
        }

        const auto openTimeMs = parseUtcTimestampToEpochMs(fields[0]);
        if (!openTimeMs) {
            continue;
        }

        try {
            candlesticks::Candlestick candle{};
            candle.openTime = *openTimeMs;
            candle.baseVolume = stod(fields[kBaseVolumeColumn]);
            candle.open = stod(fields[kOpenColumn]);
            candle.high = stod(fields[kHighColumn]);
            candle.low = stod(fields[kLowColumn]);
            candle.close = stod(fields[kCloseColumn]);
            candle.closed = true;
            const auto snapshot = engine.update(candle);
            if (seedHistory) {
                seedHistory(candle, snapshot);
            }
        } catch (const exception&) {
            continue;
        }
    }
}

}  // namespace csv_history
