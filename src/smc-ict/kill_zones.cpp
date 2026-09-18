#include "kill_zones.h"

#include "ny_time.h"

namespace smc {
namespace {

constexpr int64_t kMsPerMinute = 60 * 1000;

struct ZoneRange {
    KillZoneName name;
    int startMinute;  // inclusive, minute-of-NY-day
    int endMinute;    // exclusive, minute-of-NY-day
};

constexpr ZoneRange kZones[] = {
    {KillZoneName::London, 2 * 60, 5 * 60},        // 02:00-05:00
    {KillZoneName::NewYorkAm, 7 * 60, 10 * 60},    // 07:00-10:00
    {KillZoneName::LondonClose, 10 * 60, 12 * 60}, // 10:00-12:00
    {KillZoneName::Asian, 20 * 60, 24 * 60},       // 20:00-00:00
};

}  // namespace

optional<KillZoneName> classifyKillZone(int64_t openTimeMs) {
    const int minuteOfDay = minuteOfNewYorkDay(openTimeMs);
    for (const auto& zone : kZones) {
        if (minuteOfDay >= zone.startMinute && minuteOfDay < zone.endMinute) {
            return zone.name;
        }
    }
    return nullopt;
}

KillZoneWindow killZoneWindowFor(KillZoneName zone, int64_t referenceOpenTimeMs) {
    const int64_t dayStart = newYorkDayStartUtcMs(referenceOpenTimeMs);
    for (const auto& z : kZones) {
        if (z.name == zone) {
            KillZoneWindow window;
            window.zone = zone;
            window.startOpenTimeMs = dayStart + static_cast<int64_t>(z.startMinute) * kMsPerMinute;
            window.endOpenTimeMs = dayStart + static_cast<int64_t>(z.endMinute) * kMsPerMinute;
            return window;
        }
    }
    // Unreachable for any valid KillZoneName - all four are listed above.
    KillZoneWindow fallback;
    fallback.zone = zone;
    fallback.startOpenTimeMs = dayStart;
    fallback.endOpenTimeMs = dayStart;
    return fallback;
}

}  // namespace smc
