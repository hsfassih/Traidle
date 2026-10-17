// Standalone unit tests for src/candle_builder.{h,cpp}. Plain main(), no test
// framework (the project's only dependencies are Boost, OpenSSL and
// nlohmann-json). Exits non-zero if any check fails. Built only with
// -DTRAIDLE_BUILD_TESTS=ON; never part of the Docker image.
//
// The bucket-boundary table below was produced independently of the code under
// test, with shell integer arithmetic and GNU `date -u`, so it is a real oracle
// rather than the implementation checking itself.

#include "../src/candle_builder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace std;
using namespace candle_builder;

namespace {

int gChecks = 0;
int gFailures = 0;

void report(bool ok, const char* expr, const char* file, int line) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(cond) report(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

template <typename A, typename B>
void reportEq(const A& actual, const B& expected, const char* aExpr, const char* bExpr,
              const char* file, int line) {
    ++gChecks;
    if (!(actual == expected)) {
        ++gFailures;
        fprintf(stderr, "FAIL %s:%d: %s == %s (got %lld, expected %lld)\n", file, line, aExpr, bExpr,
                static_cast<long long>(actual), static_cast<long long>(expected));
    }
}

#define CHECK_EQ(a, b) reportEq((a), (b), #a, #b, __FILE__, __LINE__)

bool near(double a, double b) { return fabs(a - b) < 1e-12; }
#define CHECK_NEAR(a, b) report(near((a), (b)), #a " ~= " #b, __FILE__, __LINE__)

constexpr int64_t kSec = 1000;
constexpr int64_t kMin = 60 * kSec;
constexpr int64_t kHour = 60 * kMin;
constexpr int64_t kDay = 24 * kHour;

int64_t utc(int64_t y, unsigned mo, unsigned d, int h = 0, int mi = 0, int s = 0, int ms = 0) {
    return daysFromCivil(y, mo, d) * kDay + h * kHour + mi * kMin + s * kSec + ms;
}

IntervalSpec spec(const string& label) {
    auto s = intervalFromBinance(label);
    if (!s) {
        fprintf(stderr, "test bug: unknown interval %s\n", label.c_str());
        exit(2);
    }
    return *s;
}

const char* const kAllLabels[15] = {"1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h",
                                    "6h", "8h", "12h", "1d", "3d", "1w", "1M"};

// ---------------------------------------------------------------------------
// Independent oracle: {interval, timestamp, expected bucket open, expected end}
// ---------------------------------------------------------------------------
struct BucketCase {
    const char* label;
    int64_t ts;
    int64_t open;
    int64_t end;
};

const BucketCase kBucketCases[] = {
    {"1m", 1791898662123LL, 1791898620000LL, 1791898680000LL},  // 2026-10-13 13:37:42.123
    {"3m", 1791898662123LL, 1791898560000LL, 1791898740000LL},  // 2026-10-13 13:37:42.123
    {"5m", 1791898662123LL, 1791898500000LL, 1791898800000LL},  // 2026-10-13 13:37:42.123
    {"15m", 1791898662123LL, 1791898200000LL, 1791899100000LL},  // 2026-10-13 13:37:42.123
    {"30m", 1791898662123LL, 1791898200000LL, 1791900000000LL},  // 2026-10-13 13:37:42.123
    {"1h", 1791898662123LL, 1791896400000LL, 1791900000000LL},  // 2026-10-13 13:37:42.123
    {"2h", 1791898662123LL, 1791892800000LL, 1791900000000LL},  // 2026-10-13 13:37:42.123
    {"4h", 1791898662123LL, 1791892800000LL, 1791907200000LL},  // 2026-10-13 13:37:42.123
    {"6h", 1791898662123LL, 1791892800000LL, 1791914400000LL},  // 2026-10-13 13:37:42.123
    {"8h", 1791898662123LL, 1791878400000LL, 1791907200000LL},  // 2026-10-13 13:37:42.123
    {"12h", 1791898662123LL, 1791892800000LL, 1791936000000LL},  // 2026-10-13 13:37:42.123
    {"1d", 1791898662123LL, 1791849600000LL, 1791936000000LL},  // 2026-10-13 13:37:42.123
    {"3d", 1791898662123LL, 1791849600000LL, 1792108800000LL},  // 2026-10-13 13:37:42.123
    {"1w", 1791898662123LL, 1791763200000LL, 1792368000000LL},  // 2026-10-13 13:37:42.123
    {"1M", 1791898662123LL, 1790812800000LL, 1793491200000LL},  // 2026-10-13 13:37:42.123
    {"1m", 1709251199999LL, 1709251140000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"3m", 1709251199999LL, 1709251020000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"5m", 1709251199999LL, 1709250900000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"15m", 1709251199999LL, 1709250300000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"30m", 1709251199999LL, 1709249400000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"1h", 1709251199999LL, 1709247600000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"2h", 1709251199999LL, 1709244000000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"4h", 1709251199999LL, 1709236800000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"6h", 1709251199999LL, 1709229600000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"8h", 1709251199999LL, 1709222400000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"12h", 1709251199999LL, 1709208000000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"1d", 1709251199999LL, 1709164800000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"3d", 1709251199999LL, 1709164800000LL, 1709424000000LL},  // 2024-02-29 23:59:59.999
    {"1w", 1709251199999LL, 1708905600000LL, 1709510400000LL},  // 2024-02-29 23:59:59.999
    {"1M", 1709251199999LL, 1706745600000LL, 1709251200000LL},  // 2024-02-29 23:59:59.999
    {"1m", 1709251200000LL, 1709251200000LL, 1709251260000LL},  // 2024-03-01 00:00:00.000
    {"3m", 1709251200000LL, 1709251200000LL, 1709251380000LL},  // 2024-03-01 00:00:00.000
    {"5m", 1709251200000LL, 1709251200000LL, 1709251500000LL},  // 2024-03-01 00:00:00.000
    {"15m", 1709251200000LL, 1709251200000LL, 1709252100000LL},  // 2024-03-01 00:00:00.000
    {"30m", 1709251200000LL, 1709251200000LL, 1709253000000LL},  // 2024-03-01 00:00:00.000
    {"1h", 1709251200000LL, 1709251200000LL, 1709254800000LL},  // 2024-03-01 00:00:00.000
    {"2h", 1709251200000LL, 1709251200000LL, 1709258400000LL},  // 2024-03-01 00:00:00.000
    {"4h", 1709251200000LL, 1709251200000LL, 1709265600000LL},  // 2024-03-01 00:00:00.000
    {"6h", 1709251200000LL, 1709251200000LL, 1709272800000LL},  // 2024-03-01 00:00:00.000
    {"8h", 1709251200000LL, 1709251200000LL, 1709280000000LL},  // 2024-03-01 00:00:00.000
    {"12h", 1709251200000LL, 1709251200000LL, 1709294400000LL},  // 2024-03-01 00:00:00.000
    {"1d", 1709251200000LL, 1709251200000LL, 1709337600000LL},  // 2024-03-01 00:00:00.000
    {"3d", 1709251200000LL, 1709164800000LL, 1709424000000LL},  // 2024-03-01 00:00:00.000
    {"1w", 1709251200000LL, 1708905600000LL, 1709510400000LL},  // 2024-03-01 00:00:00.000
    {"1M", 1709251200000LL, 1709251200000LL, 1711929600000LL},  // 2024-03-01 00:00:00.000
    {"1m", 1767225599999LL, 1767225540000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"3m", 1767225599999LL, 1767225420000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"5m", 1767225599999LL, 1767225300000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"15m", 1767225599999LL, 1767224700000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"30m", 1767225599999LL, 1767223800000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"1h", 1767225599999LL, 1767222000000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"2h", 1767225599999LL, 1767218400000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"4h", 1767225599999LL, 1767211200000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"6h", 1767225599999LL, 1767204000000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"8h", 1767225599999LL, 1767196800000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"12h", 1767225599999LL, 1767182400000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"1d", 1767225599999LL, 1767139200000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"3d", 1767225599999LL, 1766966400000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"1w", 1767225599999LL, 1766966400000LL, 1767571200000LL},  // 2025-12-31 23:59:59.999
    {"1M", 1767225599999LL, 1764547200000LL, 1767225600000LL},  // 2025-12-31 23:59:59.999
    {"1m", 1767225600000LL, 1767225600000LL, 1767225660000LL},  // 2026-01-01 00:00:00.000
    {"3m", 1767225600000LL, 1767225600000LL, 1767225780000LL},  // 2026-01-01 00:00:00.000
    {"5m", 1767225600000LL, 1767225600000LL, 1767225900000LL},  // 2026-01-01 00:00:00.000
    {"15m", 1767225600000LL, 1767225600000LL, 1767226500000LL},  // 2026-01-01 00:00:00.000
    {"30m", 1767225600000LL, 1767225600000LL, 1767227400000LL},  // 2026-01-01 00:00:00.000
    {"1h", 1767225600000LL, 1767225600000LL, 1767229200000LL},  // 2026-01-01 00:00:00.000
    {"2h", 1767225600000LL, 1767225600000LL, 1767232800000LL},  // 2026-01-01 00:00:00.000
    {"4h", 1767225600000LL, 1767225600000LL, 1767240000000LL},  // 2026-01-01 00:00:00.000
    {"6h", 1767225600000LL, 1767225600000LL, 1767247200000LL},  // 2026-01-01 00:00:00.000
    {"8h", 1767225600000LL, 1767225600000LL, 1767254400000LL},  // 2026-01-01 00:00:00.000
    {"12h", 1767225600000LL, 1767225600000LL, 1767268800000LL},  // 2026-01-01 00:00:00.000
    {"1d", 1767225600000LL, 1767225600000LL, 1767312000000LL},  // 2026-01-01 00:00:00.000
    {"3d", 1767225600000LL, 1767225600000LL, 1767484800000LL},  // 2026-01-01 00:00:00.000
    {"1w", 1767225600000LL, 1766966400000LL, 1767571200000LL},  // 2026-01-01 00:00:00.000
    {"1M", 1767225600000LL, 1767225600000LL, 1769904000000LL},  // 2026-01-01 00:00:00.000
    {"1m", 1791763199999LL, 1791763140000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"3m", 1791763199999LL, 1791763020000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"5m", 1791763199999LL, 1791762900000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"15m", 1791763199999LL, 1791762300000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"30m", 1791763199999LL, 1791761400000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"1h", 1791763199999LL, 1791759600000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"2h", 1791763199999LL, 1791756000000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"4h", 1791763199999LL, 1791748800000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"6h", 1791763199999LL, 1791741600000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"8h", 1791763199999LL, 1791734400000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"12h", 1791763199999LL, 1791720000000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"1d", 1791763199999LL, 1791676800000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"3d", 1791763199999LL, 1791590400000LL, 1791849600000LL},  // 2026-10-11 23:59:59.999
    {"1w", 1791763199999LL, 1791158400000LL, 1791763200000LL},  // 2026-10-11 23:59:59.999
    {"1M", 1791763199999LL, 1790812800000LL, 1793491200000LL},  // 2026-10-11 23:59:59.999
    {"1m", 1791763200000LL, 1791763200000LL, 1791763260000LL},  // 2026-10-12 00:00:00.000
    {"3m", 1791763200000LL, 1791763200000LL, 1791763380000LL},  // 2026-10-12 00:00:00.000
    {"5m", 1791763200000LL, 1791763200000LL, 1791763500000LL},  // 2026-10-12 00:00:00.000
    {"15m", 1791763200000LL, 1791763200000LL, 1791764100000LL},  // 2026-10-12 00:00:00.000
    {"30m", 1791763200000LL, 1791763200000LL, 1791765000000LL},  // 2026-10-12 00:00:00.000
    {"1h", 1791763200000LL, 1791763200000LL, 1791766800000LL},  // 2026-10-12 00:00:00.000
    {"2h", 1791763200000LL, 1791763200000LL, 1791770400000LL},  // 2026-10-12 00:00:00.000
    {"4h", 1791763200000LL, 1791763200000LL, 1791777600000LL},  // 2026-10-12 00:00:00.000
    {"6h", 1791763200000LL, 1791763200000LL, 1791784800000LL},  // 2026-10-12 00:00:00.000
    {"8h", 1791763200000LL, 1791763200000LL, 1791792000000LL},  // 2026-10-12 00:00:00.000
    {"12h", 1791763200000LL, 1791763200000LL, 1791806400000LL},  // 2026-10-12 00:00:00.000
    {"1d", 1791763200000LL, 1791763200000LL, 1791849600000LL},  // 2026-10-12 00:00:00.000
    {"3d", 1791763200000LL, 1791590400000LL, 1791849600000LL},  // 2026-10-12 00:00:00.000
    {"1w", 1791763200000LL, 1791763200000LL, 1792368000000LL},  // 2026-10-12 00:00:00.000
    {"1M", 1791763200000LL, 1790812800000LL, 1793491200000LL},  // 2026-10-12 00:00:00.000
    {"1m", 1791954305005LL, 1791954300000LL, 1791954360000LL},  // 2026-10-14 05:05:05.005
    {"3m", 1791954305005LL, 1791954180000LL, 1791954360000LL},  // 2026-10-14 05:05:05.005
    {"5m", 1791954305005LL, 1791954300000LL, 1791954600000LL},  // 2026-10-14 05:05:05.005
    {"15m", 1791954305005LL, 1791954000000LL, 1791954900000LL},  // 2026-10-14 05:05:05.005
    {"30m", 1791954305005LL, 1791954000000LL, 1791955800000LL},  // 2026-10-14 05:05:05.005
    {"1h", 1791954305005LL, 1791954000000LL, 1791957600000LL},  // 2026-10-14 05:05:05.005
    {"2h", 1791954305005LL, 1791950400000LL, 1791957600000LL},  // 2026-10-14 05:05:05.005
    {"4h", 1791954305005LL, 1791950400000LL, 1791964800000LL},  // 2026-10-14 05:05:05.005
    {"6h", 1791954305005LL, 1791936000000LL, 1791957600000LL},  // 2026-10-14 05:05:05.005
    {"8h", 1791954305005LL, 1791936000000LL, 1791964800000LL},  // 2026-10-14 05:05:05.005
    {"12h", 1791954305005LL, 1791936000000LL, 1791979200000LL},  // 2026-10-14 05:05:05.005
    {"1d", 1791954305005LL, 1791936000000LL, 1792022400000LL},  // 2026-10-14 05:05:05.005
    {"3d", 1791954305005LL, 1791849600000LL, 1792108800000LL},  // 2026-10-14 05:05:05.005
    {"1w", 1791954305005LL, 1791763200000LL, 1792368000000LL},  // 2026-10-14 05:05:05.005
    {"1M", 1791954305005LL, 1790812800000LL, 1793491200000LL},  // 2026-10-14 05:05:05.005
    {"1m", 1677628799999LL, 1677628740000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"3m", 1677628799999LL, 1677628620000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"5m", 1677628799999LL, 1677628500000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"15m", 1677628799999LL, 1677627900000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"30m", 1677628799999LL, 1677627000000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"1h", 1677628799999LL, 1677625200000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"2h", 1677628799999LL, 1677621600000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"4h", 1677628799999LL, 1677614400000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"6h", 1677628799999LL, 1677607200000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"8h", 1677628799999LL, 1677600000000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"12h", 1677628799999LL, 1677585600000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"1d", 1677628799999LL, 1677542400000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"3d", 1677628799999LL, 1677542400000LL, 1677801600000LL},  // 2023-02-28 23:59:59.999
    {"1w", 1677628799999LL, 1677456000000LL, 1678060800000LL},  // 2023-02-28 23:59:59.999
    {"1M", 1677628799999LL, 1675209600000LL, 1677628800000LL},  // 2023-02-28 23:59:59.999
    {"1m", -1LL, -60000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"3m", -1LL, -180000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"5m", -1LL, -300000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"15m", -1LL, -900000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"30m", -1LL, -1800000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"1h", -1LL, -3600000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"2h", -1LL, -7200000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"4h", -1LL, -14400000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"6h", -1LL, -21600000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"8h", -1LL, -28800000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"12h", -1LL, -43200000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"1d", -1LL, -86400000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"3d", -1LL, -259200000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"1w", -1LL, -259200000LL, 345600000LL},  // 1969-12-31 23:59:59.999
    {"1M", -1LL, -2678400000LL, 0LL},  // 1969-12-31 23:59:59.999
    {"1m", 1769904000000LL, 1769904000000LL, 1769904060000LL},  // 2026-02-01 00:00:00.000
    {"3m", 1769904000000LL, 1769904000000LL, 1769904180000LL},  // 2026-02-01 00:00:00.000
    {"5m", 1769904000000LL, 1769904000000LL, 1769904300000LL},  // 2026-02-01 00:00:00.000
    {"15m", 1769904000000LL, 1769904000000LL, 1769904900000LL},  // 2026-02-01 00:00:00.000
    {"30m", 1769904000000LL, 1769904000000LL, 1769905800000LL},  // 2026-02-01 00:00:00.000
    {"1h", 1769904000000LL, 1769904000000LL, 1769907600000LL},  // 2026-02-01 00:00:00.000
    {"2h", 1769904000000LL, 1769904000000LL, 1769911200000LL},  // 2026-02-01 00:00:00.000
    {"4h", 1769904000000LL, 1769904000000LL, 1769918400000LL},  // 2026-02-01 00:00:00.000
    {"6h", 1769904000000LL, 1769904000000LL, 1769925600000LL},  // 2026-02-01 00:00:00.000
    {"8h", 1769904000000LL, 1769904000000LL, 1769932800000LL},  // 2026-02-01 00:00:00.000
    {"12h", 1769904000000LL, 1769904000000LL, 1769947200000LL},  // 2026-02-01 00:00:00.000
    {"1d", 1769904000000LL, 1769904000000LL, 1769990400000LL},  // 2026-02-01 00:00:00.000
    {"3d", 1769904000000LL, 1769817600000LL, 1770076800000LL},  // 2026-02-01 00:00:00.000
    {"1w", 1769904000000LL, 1769385600000LL, 1769990400000LL},  // 2026-02-01 00:00:00.000
    {"1M", 1769904000000LL, 1769904000000LL, 1772323200000LL},  // 2026-02-01 00:00:00.000
    {"1m", 1782863999999LL, 1782863940000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"3m", 1782863999999LL, 1782863820000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"5m", 1782863999999LL, 1782863700000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"15m", 1782863999999LL, 1782863100000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"30m", 1782863999999LL, 1782862200000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"1h", 1782863999999LL, 1782860400000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"2h", 1782863999999LL, 1782856800000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"4h", 1782863999999LL, 1782849600000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"6h", 1782863999999LL, 1782842400000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"8h", 1782863999999LL, 1782835200000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"12h", 1782863999999LL, 1782820800000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"1d", 1782863999999LL, 1782777600000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
    {"3d", 1782863999999LL, 1782777600000LL, 1783036800000LL},  // 2026-06-30 23:59:59.999
    {"1w", 1782863999999LL, 1782691200000LL, 1783296000000LL},  // 2026-06-30 23:59:59.999
    {"1M", 1782863999999LL, 1780272000000LL, 1782864000000LL},  // 2026-06-30 23:59:59.999
};

void testBucketOracle() {
    for (const auto& c : kBucketCases) {
        const IntervalSpec s = spec(c.label);
        const int64_t open = bucketOpen(s, c.ts);
        CHECK_EQ(open, c.open);
        CHECK_EQ(bucketEnd(s, open), c.end);
        // A bucket open is its own bucket, and its last millisecond too.
        CHECK_EQ(bucketOpen(s, c.open), c.open);
        CHECK_EQ(bucketOpen(s, c.end - 1), c.open);
        CHECK_EQ(bucketOpen(s, c.end), c.end);
        CHECK(c.ts >= open && c.ts < c.end);
    }
}

void testIntervalTable() {
    for (const char* label : kAllLabels) {
        CHECK(intervalFromBinance(label).has_value());
    }
    CHECK(!intervalFromBinance("1mo").has_value());
    CHECK(!intervalFromBinance("7m").has_value());
    CHECK(!intervalFromBinance("").has_value());
    CHECK(!intervalFromBinance("1H").has_value());
    // "1m" (minute) and "1M" (month) are different.
    CHECK(spec("1m").kind == IntervalSpec::Kind::Fixed);
    CHECK(spec("1M").kind == IntervalSpec::Kind::Month);
    CHECK(spec("1w").kind == IntervalSpec::Kind::Week);
}

void testSpecificBoundaries() {
    // Leap-year February: Feb 29 exists in 2024, not in 2023 or 2100.
    const IntervalSpec month = spec("1M");
    CHECK_EQ(bucketOpen(month, utc(2024, 2, 15, 12)), utc(2024, 2, 1));
    CHECK_EQ(bucketEnd(month, utc(2024, 2, 1)), utc(2024, 3, 1));
    CHECK_EQ(bucketEnd(month, utc(2024, 2, 1)) - utc(2024, 2, 1), 29 * kDay);
    CHECK_EQ(bucketEnd(month, utc(2023, 2, 1)) - utc(2023, 2, 1), 28 * kDay);
    CHECK_EQ(bucketEnd(month, utc(2100, 2, 1)) - utc(2100, 2, 1), 28 * kDay);  // 2100 is not a leap year
    CHECK_EQ(bucketEnd(month, utc(2000, 2, 1)) - utc(2000, 2, 1), 29 * kDay);  // 2000 is
    // Year boundary.
    CHECK_EQ(bucketOpen(month, utc(2025, 12, 31, 23, 59, 59, 999)), utc(2025, 12, 1));
    CHECK_EQ(bucketEnd(month, utc(2025, 12, 1)), utc(2026, 1, 1));
    CHECK_EQ(bucketOpen(month, utc(2026, 1, 1)), utc(2026, 1, 1));
    CHECK_EQ(bucketOpen(spec("1d"), utc(2025, 12, 31, 23, 59, 59, 999)), utc(2025, 12, 31));
    CHECK_EQ(bucketOpen(spec("1d"), utc(2026, 1, 1)), utc(2026, 1, 1));
    // Monday week boundary: 2026-10-12 is a Monday.
    const IntervalSpec week = spec("1w");
    CHECK_EQ(bucketOpen(week, utc(2026, 10, 12)), utc(2026, 10, 12));
    CHECK_EQ(bucketOpen(week, utc(2026, 10, 11, 23, 59, 59, 999)), utc(2026, 10, 5));
    CHECK_EQ(bucketOpen(week, utc(2026, 10, 18, 23, 59, 59, 999)), utc(2026, 10, 12));
    CHECK_EQ(bucketEnd(week, utc(2026, 10, 12)), utc(2026, 10, 19));
    CHECK_EQ(bucketOpen(week, 345600000LL), 345600000LL);  // 1970-01-05, the first Monday
    CHECK_EQ(bucketOpen(week, 0), -3 * kDay);               // Thursday 1970-01-01 -> Monday 1969-12-29
    // 3d: multiples of 3 epoch-days.
    const IntervalSpec threeDay = spec("3d");
    CHECK_EQ(bucketOpen(threeDay, 0), 0);
    CHECK_EQ(bucketOpen(threeDay, 2 * kDay + 5), 0);
    CHECK_EQ(bucketOpen(threeDay, 3 * kDay), 3 * kDay);
    CHECK_EQ(bucketOpen(threeDay, -1), -3 * kDay);
    // 20739 is divisible by 3, so 2026-10-13 opens a 3d bucket; the 14th and 15th belong to it.
    CHECK_EQ(bucketOpen(threeDay, utc(2026, 10, 15, 23, 59)), utc(2026, 10, 13));
    CHECK_EQ(bucketOpen(threeDay, utc(2026, 10, 16)), utc(2026, 10, 16));
    // Every bucket of every interval tiles time: end(open) is the next bucket's open.
    for (const char* label : kAllLabels) {
        const IntervalSpec s = spec(label);
        int64_t ts = utc(2023, 12, 20);
        for (int i = 0; i < 4000; ++i) {
            const int64_t open = bucketOpen(s, ts);
            const int64_t end = bucketEnd(s, open);
            CHECK(end > open);
            CHECK_EQ(bucketOpen(s, end), end);
            CHECK_EQ(bucketOpen(s, end - 1), open);
            ts = end + (i % 7) * kMin;  // hop forward bucket by bucket
        }
    }
}

void testCalendarHelpers() {
    CHECK_EQ(daysFromCivil(1970, 1, 1), 0);
    CHECK_EQ(utc(2024, 2, 29), 1709164800LL * 1000);
    CHECK_EQ(utc(2000, 1, 1), 946684800LL * 1000);
    CHECK_EQ(utc(1900, 3, 1), -2203891200LL * 1000);
    CHECK_EQ(utc(2026, 10, 13), 1791849600LL * 1000);
    // Round trip every day from 1600 to 2400.
    for (int64_t day = daysFromCivil(1600, 1, 1); day <= daysFromCivil(2400, 12, 31); ++day) {
        const CivilDate d = civilFromDays(day);
        CHECK_EQ(daysFromCivil(d.year, d.month, d.day), day);
        CHECK(d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= daysInMonth(d.year, d.month));
    }
    CHECK(isLeapYear(2024));
    CHECK(!isLeapYear(2023));
    CHECK(!isLeapYear(1900));
    CHECK(isLeapYear(2000));
    CHECK_EQ(floorDiv(-1, 1000), -1);
    CHECK_EQ(floorDiv(-1000, 1000), -1);
    CHECK_EQ(floorDiv(-1001, 1000), -2);
    CHECK_EQ(floorDiv(1999, 1000), 1);
    CHECK_EQ(floorMod(-1, 7), 6);
    CHECK_EQ(floorMod(14, 7), 0);

    CHECK_EQ(*parseUtcIso("2024-02-29T00:00:00"), 1709164800LL * 1000);
    CHECK_EQ(*parseUtcIso("2026-10-13T13:37:42"), utc(2026, 10, 13, 13, 37, 42));
    CHECK_EQ(*parseUtcIso("2026-10-13 13:37:42"), utc(2026, 10, 13, 13, 37, 42));
    CHECK_EQ(*parseUtcIso("2026-10-13T13:37:42.5Z"), utc(2026, 10, 13, 13, 37, 42, 500));
    CHECK_EQ(*parseUtcIso("2026-10-13T13:37:42.123456"), utc(2026, 10, 13, 13, 37, 42, 123));
    CHECK(!parseUtcIso("2023-02-29T00:00:00").has_value());  // not a leap year
    CHECK(!parseUtcIso("2026-13-01T00:00:00").has_value());
    CHECK(!parseUtcIso("2026-10-13T24:00:00").has_value());
    CHECK(!parseUtcIso("2026-10-13T13:37").has_value());
    CHECK(!parseUtcIso("2026-10-13T13:37:42junk").has_value());
    CHECK(!parseUtcIso("").has_value());
    CHECK(!parseUtcIso("20261013T133742").has_value());
    CHECK(formatUtcIso(utc(2026, 10, 13, 13, 37, 42, 999)) == "2026-10-13T13:37:42");
    CHECK(formatUtcIso(0) == "1970-01-01T00:00:00");
    CHECK(formatUtcIso(-1) == "1969-12-31T23:59:59");
    CHECK(formatUtcIso(utc(2024, 2, 29, 23, 59, 59)) == "2024-02-29T23:59:59");
    for (int64_t ms = utc(2023, 12, 31, 23, 59, 50); ms < utc(2024, 1, 1, 0, 0, 10); ms += 997) {
        CHECK_EQ(*parseUtcIso(formatUtcIso(ms)), floorDiv(ms, 1000) * 1000);
    }
}

void testBaseSources() {
    struct Row {
        const char* label;
        const char* resolution;
        int64_t stepMs;
        int needed;
    };
    const Row rows[] = {
        {"1m", "MINUTE", kMin, 2},        {"3m", "MINUTE", kMin, 4},
        {"5m", "MINUTE_5", 5 * kMin, 2},  {"15m", "MINUTE_15", 15 * kMin, 2},
        {"30m", "MINUTE_30", 30 * kMin, 2}, {"1h", "HOUR", kHour, 2},
        {"2h", "HOUR", kHour, 3},         {"4h", "HOUR_4", 4 * kHour, 2},
        {"6h", "HOUR", kHour, 7},         {"8h", "HOUR_4", 4 * kHour, 3},
        {"12h", "HOUR_4", 4 * kHour, 4},  {"1d", "DAY", kDay, 2},
        {"3d", "DAY", kDay, 4},           {"1w", "WEEK", 7 * kDay, 2},
        {"1M", "DAY", kDay, 32},
    };
    for (const auto& row : rows) {
        const IntervalSpec s = spec(row.label);
        const BaseSource b = baseSourceFor(s);
        CHECK(b.capitalResolution == row.resolution);
        CHECK_EQ(b.nominalStepMs, row.stepMs);
        CHECK_EQ(neededBaseBars(s), row.needed);
        // The base step must divide the bucket (or the base bars would straddle buckets).
        if (s.kind == IntervalSpec::Kind::Fixed) {
            CHECK_EQ(s.fixedMs % b.nominalStepMs, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// TickCandleBuilder
// ---------------------------------------------------------------------------
void testTickSequenceAndVolume() {
    TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
    const int64_t t0 = utc(2026, 10, 13, 10, 5);
    auto r = b.onTick(t0 + 1 * kSec, 1.1000);
    CHECK(r.forming.has_value() && !r.closed.has_value());
    CHECK(!r.forming->closed);
    CHECK_EQ(r.forming->openTime, t0);
    CHECK_EQ(r.forming->closeTime, t0 + kMin - 1);
    CHECK(r.forming->symbol == "EURUSD");
    b.onTick(t0 + 10 * kSec, 1.1010);
    b.onTick(t0 + 20 * kSec, 1.0990);
    r = b.onTick(t0 + 30 * kSec, 1.1005);
    CHECK(!r.closed.has_value());
    CHECK_NEAR(r.forming->open, 1.1000);
    CHECK_NEAR(r.forming->high, 1.1010);
    CHECK_NEAR(r.forming->low, 1.0990);
    CHECK_NEAR(r.forming->close, 1.1005);
    CHECK_NEAR(r.forming->baseVolume, 4.0);  // tick count
    CHECK_NEAR(r.forming->quoteVolume, 0.0);
    CHECK_NEAR(r.forming->takerBuyBaseVolume, 0.0);
    CHECK_NEAR(r.forming->takerBuyQuoteVolume, 0.0);

    // Rollover emits exactly one closed candle, then opens the next from this tick.
    r = b.onTick(t0 + kMin + 2 * kSec, 1.1020);
    CHECK(r.closed.has_value());
    CHECK(r.closed->closed);
    CHECK_EQ(r.closed->openTime, t0);
    CHECK_EQ(r.closed->closeTime, t0 + kMin - 1);
    CHECK_NEAR(r.closed->open, 1.1000);
    CHECK_NEAR(r.closed->high, 1.1010);
    CHECK_NEAR(r.closed->low, 1.0990);
    CHECK_NEAR(r.closed->close, 1.1005);
    CHECK_NEAR(r.closed->baseVolume, 4.0);
    CHECK(r.closedPartial);  // cold start: the very first candle was never observed from its start
    CHECK(r.forming.has_value() && !r.forming->closed);
    CHECK_EQ(r.forming->openTime, t0 + kMin);
    CHECK_NEAR(r.forming->open, 1.1020);
    CHECK_NEAR(r.forming->baseVolume, 1.0);
    r = b.onTick(t0 + kMin + 5 * kSec, 1.1022);
    CHECK(!r.closed.has_value());  // no second close for the same rollover

    // Volume scale.
    TickCandleBuilder scaled(spec("1m"), "EURUSD", 2.5, 1500);
    scaled.onTick(t0, 1.0);
    r = scaled.onTick(t0 + 1, 1.0);
    CHECK_NEAR(r.forming->baseVolume, 5.0);
}

void testInvalidTicksIgnored() {
    TickCandleBuilder b(spec("1m"), "EURUSD");
    const int64_t t0 = utc(2026, 10, 13, 10, 5);
    for (double bad : {0.0, -1.0, numeric_limits<double>::quiet_NaN(),
                       numeric_limits<double>::infinity(), -numeric_limits<double>::infinity()}) {
        auto r = b.onTick(t0, bad);
        CHECK(!r.forming.has_value() && !r.closed.has_value() && !r.droppedLate);
    }
    CHECK(!b.hasCurrent());
    CHECK_EQ(b.lateDropped(), 0u);
}

void testLateTicks() {
    TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
    const int64_t t0 = utc(2026, 10, 13, 10, 5);
    b.onTick(t0 + 5 * kSec, 1.10);
    b.onTick(t0 + kMin + 1 * kSec, 1.11);  // closes minute 0
    auto r = b.onTick(t0 + 50 * kSec, 1.50);  // straggler from the closed minute
    CHECK(r.droppedLate);
    CHECK(!r.forming.has_value() && !r.closed.has_value());
    CHECK_EQ(b.lateDropped(), 1u);
    // The late tick must not have changed the current candle.
    r = b.onTick(t0 + kMin + 2 * kSec, 1.112);
    CHECK_NEAR(r.forming->high, 1.112);
    CHECK_NEAR(r.forming->low, 1.11);
    CHECK_NEAR(r.forming->baseVolume, 2.0);
}

void testFlush() {
    TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
    const int64_t t0 = utc(2026, 10, 13, 10, 5);
    CHECK(!b.flush(t0 + 10 * kHour).has_value());  // nothing open
    b.onTick(t0 + 5 * kSec, 1.10);
    b.onTick(t0 + 40 * kSec, 1.12);
    const int64_t end = t0 + kMin;
    CHECK(!b.flush(end).has_value());
    CHECK(!b.flush(end + 1499).has_value());
    auto closed = b.flush(end + 1500);
    CHECK(closed.has_value());
    CHECK(closed->closed);
    CHECK_EQ(closed->openTime, t0);
    CHECK_EQ(closed->closeTime, end - 1);
    CHECK_NEAR(closed->high, 1.12);
    CHECK_NEAR(closed->baseVolume, 2.0);
    CHECK(!b.hasCurrent());
    CHECK(!b.flush(end + 5000).has_value());  // no duplicate

    // A straggler for the flushed bucket must not reopen it.
    auto r = b.onTick(t0 + 59 * kSec, 1.30);
    CHECK(r.droppedLate);
    CHECK(!b.hasCurrent());
    CHECK_EQ(b.lateDropped(), 1u);

    // A tick in the next bucket opens it normally, with no second close of the old one.
    r = b.onTick(end + 3 * kSec, 1.13);
    CHECK(!r.closed.has_value());
    CHECK(r.forming.has_value());
    CHECK_EQ(r.forming->openTime, end);
    CHECK_NEAR(r.forming->baseVolume, 1.0);
}

void testPartialTracking() {
    const int64_t t0 = utc(2026, 10, 13, 10, 5);
    {   // Cold start: first candle is partial, the next is not.
        TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
        b.onTick(t0 + 30 * kSec, 1.10);
        auto r = b.onTick(t0 + kMin + 1, 1.11);
        CHECK(r.closed.has_value() && r.closedPartial && b.lastClosedPartial());
        r = b.onTick(t0 + 2 * kMin + 1, 1.12);
        CHECK(r.closed.has_value() && !r.closedPartial && !b.lastClosedPartial());
        auto flushed = b.flush(t0 + 3 * kMin + 1500);
        CHECK(flushed.has_value() && !b.lastClosedPartial());
    }
    {   // A flush-closed cold candle is also reported partial.
        TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
        b.onTick(t0 + 30 * kSec, 1.10);
        auto flushed = b.flush(t0 + kMin + 1500);
        CHECK(flushed.has_value() && b.lastClosedPartial());
        b.onTick(t0 + kMin + 2 * kSec, 1.11);
        flushed = b.flush(t0 + 2 * kMin + 1500);
        CHECK(flushed.has_value() && !b.lastClosedPartial());  // opened after we were listening
    }
    {   // Seeded candle is complete; ticks continue it.
        TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
        Bar seed{t0, 1.1000, 1.1010, 1.0990, 1.1005, 17.0};
        CHECK(b.seed(seed, t0 + 20 * kSec));
        auto r = b.onTick(t0 + 10 * kSec, 9.99);  // before asOf: already contained in the seed
        CHECK(!r.forming.has_value() && !r.droppedLate);
        r = b.onTick(t0 + 25 * kSec, 1.1020);
        CHECK_NEAR(r.forming->open, 1.1000);
        CHECK_NEAR(r.forming->high, 1.1020);
        CHECK_NEAR(r.forming->low, 1.0990);
        CHECK_NEAR(r.forming->close, 1.1020);
        CHECK_NEAR(r.forming->baseVolume, 18.0);
        r = b.onTick(t0 + kMin + 1, 1.1030);
        CHECK(r.closed.has_value() && !r.closedPartial);
        CHECK_NEAR(r.closed->baseVolume, 18.0);
    }
    {   // An invalid seed changes nothing.
        TickCandleBuilder b(spec("1m"), "EURUSD");
        CHECK(!b.seed(Bar{t0, 0.0, 1.0, 1.0, 1.0, 1.0}, t0));
        CHECK(!b.seed(Bar{t0, 1.0, numeric_limits<double>::quiet_NaN(), 1.0, 1.0, 1.0}, t0));
        CHECK(!b.hasCurrent());
    }
    {   // peekForming mirrors the current candle and changes nothing.
        TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
        CHECK(!b.peekForming().has_value());
        b.onTick(t0 + 5 * kSec, 1.10);
        b.onTick(t0 + 6 * kSec, 1.12);
        auto peek = b.peekForming();
        CHECK(peek.has_value() && !peek->closed);
        CHECK_NEAR(peek->high, 1.12);
        CHECK_NEAR(peek->baseVolume, 2.0);
        auto r = b.onTick(t0 + 7 * kSec, 1.11);
        CHECK_NEAR(r.forming->baseVolume, 3.0);  // peek did not consume anything
    }
    {   // A seed for a bucket this builder already closed is refused (no duplicate close later).
        TickCandleBuilder b(spec("1m"), "EURUSD", 1.0, 1500);
        b.onTick(t0 + 5 * kSec, 1.10);
        CHECK(b.flush(t0 + kMin + 1500).has_value());
        CHECK(!b.seed(Bar{t0, 1.10, 1.11, 1.09, 1.10, 4.0}, t0 + kMin));
        CHECK(!b.hasCurrent());
        CHECK(b.seed(Bar{t0 + kMin, 1.10, 1.11, 1.09, 1.10, 4.0}, t0 + kMin));  // the next bucket is fine
        CHECK(b.hasCurrent());
    }
    {   // A seed that is slightly inconsistent is normalized to keep the OHLC invariant.
        TickCandleBuilder b(spec("1m"), "EURUSD");
        CHECK(b.seed(Bar{t0, 1.20, 1.10, 1.30, 1.25, 1.0}, t0));
        auto r = b.onTick(t0 + 1 * kSec, 1.21);
        CHECK(r.forming->high >= max(r.forming->open, r.forming->close));
        CHECK(r.forming->low <= min(r.forming->open, r.forming->close));
    }
    {   // markCovered: REST says the bucket was empty, so the first tick starts a complete candle.
        TickCandleBuilder b(spec("1m"), "EURUSD");
        b.markCovered(t0 + 20 * kSec);
        b.onTick(t0 + 40 * kSec, 1.10);
        auto r = b.onTick(t0 + kMin + 1, 1.11);
        CHECK(r.closed.has_value() && !r.closedPartial);
    }
    {   // invalidate (reconnect with a failed re-seed): current + next are partial, then normal.
        TickCandleBuilder b(spec("1m"), "EURUSD");
        b.onTick(t0 + 1 * kSec, 1.10);
        b.onTick(t0 + kMin + 1, 1.11);       // closes the cold candle (partial)
        b.invalidate();
        auto r = b.onTick(t0 + 2 * kMin + 1, 1.12);
        CHECK(r.closed.has_value() && r.closedPartial);   // the invalidated candle
        r = b.onTick(t0 + 3 * kMin + 1, 1.13);
        CHECK(r.closed.has_value() && r.closedPartial);   // the one opened right after the gap
        r = b.onTick(t0 + 4 * kMin + 1, 1.14);
        CHECK(r.closed.has_value() && !r.closedPartial);  // continuity restored
    }
    {   // invalidate then a successful re-seed makes the candle complete again.
        TickCandleBuilder b(spec("1m"), "EURUSD");
        b.onTick(t0 + 1 * kSec, 1.10);
        b.invalidate();
        CHECK(b.seed(Bar{t0, 1.10, 1.105, 1.099, 1.101, 6.0}, t0 + 30 * kSec));
        auto r = b.onTick(t0 + kMin + 1, 1.11);
        CHECK(r.closed.has_value() && !r.closedPartial);
    }
}

void testMonthlyBuilder() {
    TickCandleBuilder b(spec("1M"), "XAUUSD", 1.0, 1500);
    b.onTick(utc(2024, 2, 10), 2000.0);
    auto r = b.onTick(utc(2024, 2, 29, 23, 59, 59, 900), 2010.0);
    CHECK(!r.closed.has_value());
    r = b.onTick(utc(2024, 3, 1, 0, 0, 0, 1), 2020.0);
    CHECK(r.closed.has_value());
    CHECK_EQ(r.closed->openTime, utc(2024, 2, 1));
    CHECK_EQ(r.closed->closeTime, utc(2024, 3, 1) - 1);
    CHECK_EQ(r.forming->openTime, utc(2024, 3, 1));
    CHECK_EQ(r.forming->closeTime, utc(2024, 4, 1) - 1);
}

// Deterministic LCG so the "random" test is reproducible on every platform.
struct Lcg {
    uint64_t state;
    uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 33;
    }
    double unit() { return static_cast<double>(next() % 1000000) / 1000000.0; }
};

void checkCandleInvariant(const candlesticks::Candlestick& c) {
    CHECK(c.high >= max(c.open, c.close));
    CHECK(c.low <= min(c.open, c.close));
    CHECK(c.low > 0);
    CHECK(c.closeTime >= c.openTime);
}

void testRandomizedInvariants() {
    for (const char* label : {"1m", "5m", "1h", "4h", "1d", "1w", "1M", "3m", "3d"}) {
        const IntervalSpec s = spec(label);
        TickCandleBuilder b(s, "EURUSD", 1.0, 1500);
        Lcg rng{0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(s.label[0])};
        int64_t ts = utc(2024, 1, 1);
        double price = 1.1000;
        optional<int64_t> lastClosedOpen;
        uint64_t closedCount = 0;
        for (int i = 0; i < 100000; ++i) {
            // Mostly small forward steps, sometimes a big jump (weekend / quiet market),
            // sometimes a small step backwards (an out-of-order quote).
            const double dice = rng.unit();
            if (dice < 0.93) {
                ts += static_cast<int64_t>(rng.unit() * 4000);
            } else if (dice < 0.97) {
                ts += static_cast<int64_t>(rng.unit() * 3 * kDay);
            } else {
                ts -= static_cast<int64_t>(rng.unit() * 90 * kSec);
            }
            price *= 1.0 + (rng.unit() - 0.5) * 0.0004;
            if (price < 0.5) price = 0.5;

            auto r = b.onTick(ts, price);
            if (r.closed) {
                checkCandleInvariant(*r.closed);
                CHECK(r.closed->closed);
                CHECK_EQ(r.closed->closeTime, bucketEnd(s, r.closed->openTime) - 1);
                if (lastClosedOpen) {
                    CHECK(r.closed->openTime > *lastClosedOpen);  // strictly increasing, no duplicates
                }
                lastClosedOpen = r.closed->openTime;
                ++closedCount;
            }
            if (r.forming) {
                checkCandleInvariant(*r.forming);
                CHECK(!r.forming->closed);
                CHECK_EQ(r.forming->openTime, bucketOpen(s, r.forming->openTime));
                if (lastClosedOpen) {
                    CHECK(r.forming->openTime > *lastClosedOpen);
                }
            }
            if (i % 37 == 0) {
                // The timer: a server clock a bit ahead of the quote stream.
                if (auto f = b.flush(ts + static_cast<int64_t>(rng.unit() * 10 * kMin))) {
                    checkCandleInvariant(*f);
                    CHECK(f->closed);
                    if (lastClosedOpen) {
                        CHECK(f->openTime > *lastClosedOpen);
                    }
                    lastClosedOpen = f->openTime;
                    ++closedCount;
                }
            }
        }
        CHECK(closedCount > 10);
        CHECK(b.ticksAccepted() + b.lateDropped() <= 100000u);
    }
}

// ---------------------------------------------------------------------------
// aggregate / completeness
// ---------------------------------------------------------------------------
Bar bar(int64_t open, double o, double h, double l, double c, double v) {
    return Bar{open, o, h, l, c, v};
}

void testAggregateMinutes() {
    const int64_t t0 = utc(2026, 10, 13, 10, 0);  // 10:00, divisible by 3 minutes
    // 3m from 1m: bucket [10:00,10:03) has all three bars, [10:03,10:06) lacks 10:04, and
    // [10:06,10:09) is missing entirely (a gap that must stay a gap).
    vector<Bar> base = {
        bar(t0 + 0 * kMin, 1.00, 1.05, 0.99, 1.02, 3),
        bar(t0 + 1 * kMin, 1.02, 1.08, 1.01, 1.07, 4),
        bar(t0 + 2 * kMin, 1.07, 1.09, 1.00, 1.03, 5),
        bar(t0 + 3 * kMin, 1.03, 1.04, 1.02, 1.03, 6),
        bar(t0 + 5 * kMin, 1.03, 1.10, 1.03, 1.09, 7),
        bar(t0 + 9 * kMin, 1.09, 1.20, 1.08, 1.15, 8),
    };
    const auto out = aggregate(base, spec("3m"));
    CHECK_EQ(out.size(), 3u);
    CHECK_EQ(out[0].openMs, t0);
    CHECK_NEAR(out[0].o, 1.00);
    CHECK_NEAR(out[0].h, 1.09);
    CHECK_NEAR(out[0].l, 0.99);
    CHECK_NEAR(out[0].c, 1.03);
    CHECK_NEAR(out[0].volume, 12.0);
    CHECK_EQ(out[1].openMs, t0 + 3 * kMin);
    CHECK_NEAR(out[1].o, 1.03);
    CHECK_NEAR(out[1].h, 1.10);
    CHECK_NEAR(out[1].l, 1.02);
    CHECK_NEAR(out[1].c, 1.09);
    CHECK_NEAR(out[1].volume, 13.0);
    CHECK_EQ(out[2].openMs, t0 + 9 * kMin);  // 10:06 bucket absent: no synthetic bar
    CHECK_NEAR(out[2].volume, 8.0);

    // Identity for a native interval keeps every bar as-is.
    const auto same = aggregate(base, spec("1m"));
    CHECK_EQ(same.size(), base.size());
    for (size_t i = 0; i < base.size(); ++i) {
        CHECK_EQ(same[i].openMs, base[i].openMs);
        CHECK_NEAR(same[i].volume, base[i].volume);
        CHECK_NEAR(same[i].c, base[i].c);
    }
    CHECK(aggregate({}, spec("3m")).empty());
}

void testAggregateHours() {
    const int64_t day = utc(2026, 10, 13);
    vector<Bar> hours;
    for (int h = 0; h < 24; ++h) {
        hours.push_back(bar(day + h * kHour, 100 + h, 101 + h, 99 + h, 100.5 + h, 10));
    }
    const auto two = aggregate(hours, spec("2h"));
    CHECK_EQ(two.size(), 12u);
    CHECK_EQ(two[1].openMs, day + 2 * kHour);
    CHECK_NEAR(two[1].o, 102);
    CHECK_NEAR(two[1].c, 103.5);
    CHECK_NEAR(two[1].h, 104);
    CHECK_NEAR(two[1].l, 101);
    CHECK_NEAR(two[1].volume, 20);
    const auto six = aggregate(hours, spec("6h"));
    CHECK_EQ(six.size(), 4u);
    CHECK_NEAR(six[2].o, 112);
    CHECK_NEAR(six[2].c, 117.5);
    CHECK_NEAR(six[2].volume, 60);
    // 8h / 12h from 4-hour bars (00,04,08,12,16,20).
    vector<Bar> fours;
    for (int i = 0; i < 6; ++i) {
        fours.push_back(bar(day + i * 4 * kHour, 200 + i, 205 + i, 195 + i, 201 + i, 100));
    }
    const auto eight = aggregate(fours, spec("8h"));
    CHECK_EQ(eight.size(), 3u);
    CHECK_EQ(eight[1].openMs, day + 8 * kHour);
    CHECK_NEAR(eight[1].o, 202);
    CHECK_NEAR(eight[1].c, 204);
    CHECK_NEAR(eight[1].volume, 200);
    const auto twelve = aggregate(fours, spec("12h"));
    CHECK_EQ(twelve.size(), 2u);
    CHECK_EQ(twelve[1].openMs, day + 12 * kHour);
    CHECK_NEAR(twelve[1].o, 203);
    CHECK_NEAR(twelve[1].c, 206);
    CHECK_NEAR(twelve[1].h, 210);
    CHECK_NEAR(twelve[1].l, 198);
    CHECK_NEAR(twelve[1].volume, 300);
    // 4h from 4-hour bars is the identity even if some are missing.
    fours.erase(fours.begin() + 2);
    CHECK_EQ(aggregate(fours, spec("4h")).size(), 5u);
}

void testAggregateDaysWeeksMonths() {
    // Business days 2026-10-05 (Mon) .. 2026-10-16 (Fri), weekend gap in between.
    vector<Bar> days;
    int n = 0;
    for (int d = 5; d <= 16; ++d) {
        const int64_t open = utc(2026, 10, d);
        const int dow = static_cast<int>(floorMod(floorDiv(open, kDay) - 4, 7));  // 0 = Monday
        if (dow >= 5) continue;
        days.push_back(bar(open, 1.0 + n, 2.0 + n, 0.5 + n, 1.5 + n, 10));
        ++n;
    }
    CHECK_EQ(days.size(), 10u);
    const auto weeks = aggregate(days, spec("1w"));
    CHECK_EQ(weeks.size(), 2u);
    CHECK_EQ(weeks[0].openMs, utc(2026, 10, 5));
    CHECK_NEAR(weeks[0].volume, 50);
    CHECK_NEAR(weeks[0].o, 1.0);
    CHECK_NEAR(weeks[0].c, 5.5);  // Friday's close
    CHECK_EQ(weeks[1].openMs, utc(2026, 10, 12));

    // 3d from DAY: epoch-day multiples of 3. 20739 (2026-10-13) opens a bucket.
    const auto threeDay = aggregate(days, spec("3d"));
    for (size_t i = 1; i < threeDay.size(); ++i) {
        CHECK(threeDay[i].openMs > threeDay[i - 1].openMs);
    }
    double volume = 0;
    for (const auto& b : threeDay) {
        CHECK_EQ(floorMod(floorDiv(b.openMs, kDay), 3), 0);
        volume += b.volume;
    }
    CHECK_NEAR(volume, 100);  // nothing lost, nothing invented

    // 1M across a leap February and the year boundary.
    vector<Bar> mdays = {
        bar(utc(2024, 1, 31), 10, 11, 9, 10.5, 1),
        bar(utc(2024, 2, 1), 20, 22, 19, 21, 2),
        bar(utc(2024, 2, 29), 30, 33, 28, 31, 3),
        bar(utc(2024, 3, 1), 40, 41, 39, 40.5, 4),
        bar(utc(2024, 12, 31), 50, 52, 49, 51, 5),
        bar(utc(2025, 1, 2), 60, 61, 58, 59, 6),
    };
    const auto months = aggregate(mdays, spec("1M"));
    CHECK_EQ(months.size(), 5u);
    CHECK_EQ(months[0].openMs, utc(2024, 1, 1));
    CHECK_EQ(months[1].openMs, utc(2024, 2, 1));
    CHECK_NEAR(months[1].o, 20);
    CHECK_NEAR(months[1].h, 33);
    CHECK_NEAR(months[1].l, 19);
    CHECK_NEAR(months[1].c, 31);
    CHECK_NEAR(months[1].volume, 5);
    CHECK_EQ(months[2].openMs, utc(2024, 3, 1));
    CHECK_EQ(months[3].openMs, utc(2024, 12, 1));
    CHECK_EQ(months[4].openMs, utc(2025, 1, 1));
}

void testCompletenessRules() {
    const IntervalSpec h2 = spec("2h");
    const int64_t day = utc(2026, 10, 13);
    // Hourly bars 06:00..13:00 (the 13:00 one is still forming at 13:20).
    vector<Bar> hours;
    for (int h = 6; h <= 13; ++h) {
        hours.push_back(bar(day + h * kHour, 1, 2, 0.5, 1.5, 1));
    }
    const auto agg = aggregate(hours, h2);  // buckets 06,08,10,12 ; 12 holds only 12:00 and 13:00
    CHECK_EQ(agg.size(), 4u);

    SelectOptions options;
    options.nowMs = day + 13 * kHour + 20 * kMin;
    options.safetyMarginMs = 2000;
    auto kept = selectClosedBuckets(agg, h2, options);
    CHECK_EQ(kept.size(), 3u);  // 12:00-14:00 is still forming: dropped
    CHECK_EQ(kept.back().openMs, day + 10 * kHour);

    // Bucket ends exactly on the safe boundary: kept; one ms short: dropped.
    options.nowMs = day + 14 * kHour + 2000;
    CHECK_EQ(selectClosedBuckets(agg, h2, options).size(), 4u);
    options.nowMs = day + 14 * kHour + 1999;
    CHECK_EQ(selectClosedBuckets(agg, h2, options).size(), 3u);

    // dropFirstBucket and the already-stored filter.
    options.nowMs = day + 20 * kHour;
    options.dropFirstBucket = true;
    kept = selectClosedBuckets(agg, h2, options);
    CHECK_EQ(kept.size(), 3u);
    CHECK_EQ(kept.front().openMs, day + 8 * kHour);
    options.dropFirstBucket = false;
    options.afterOpenMs = day + 8 * kHour;  // buckets with open <= 08:00 are already in the CSV
    kept = selectClosedBuckets(agg, h2, options);
    CHECK_EQ(kept.size(), 2u);
    CHECK_EQ(kept.front().openMs, day + 10 * kHour);

    // firstBucketMayBePartial
    const int64_t lower = day + 6 * kHour;
    CHECK(!firstBucketMayBePartial({}, h2, lower));
    // Walk reached the resume point: complete.
    CHECK(!firstBucketMayBePartial({bar(day + 5 * kHour, 1, 1, 1, 1, 1)}, h2, lower));
    // Ran out of data at 07:00, mid-bucket (bucket opens 06:00): may be partial.
    CHECK(firstBucketMayBePartial({bar(day + 7 * kHour, 1, 1, 1, 1, 1)}, h2, lower - 2 * kHour));
    // Ran out of data exactly on a bucket boundary: complete.
    CHECK(!firstBucketMayBePartial({bar(day + 8 * kHour, 1, 1, 1, 1, 1)}, h2, lower - 2 * kHour));
    // Native resolution is never partial.
    CHECK(!firstBucketMayBePartial({bar(day + 7 * kHour, 1, 1, 1, 1, 1)}, spec("1h"), lower - 2 * kHour));
}

void testToCandlestick() {
    const IntervalSpec s = spec("1M");
    const Bar b = bar(utc(2024, 2, 1), 1.1, 1.3, 1.0, 1.2, 77);
    const auto c = toCandlestick(b, s, "EURUSD", true);
    CHECK(c.symbol == "EURUSD");
    CHECK_EQ(c.openTime, utc(2024, 2, 1));
    CHECK_EQ(c.closeTime, utc(2024, 3, 1) - 1);
    CHECK(c.closed);
    CHECK_NEAR(c.baseVolume, 77);
    CHECK_NEAR(c.quoteVolume, 0);
    CHECK_NEAR(c.takerBuyBaseVolume, 0);
    CHECK_NEAR(c.takerBuyQuoteVolume, 0);
}

}  // namespace

int main() {
    testBucketOracle();
    testIntervalTable();
    testSpecificBoundaries();
    testCalendarHelpers();
    testBaseSources();
    testTickSequenceAndVolume();
    testInvalidTicksIgnored();
    testLateTicks();
    testFlush();
    testPartialTracking();
    testMonthlyBuilder();
    testRandomizedInvariants();
    testAggregateMinutes();
    testAggregateHours();
    testAggregateDaysWeeksMonths();
    testCompletenessRules();
    testToCandlestick();

    printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}