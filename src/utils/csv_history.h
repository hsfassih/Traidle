#pragma once

#include "candlesticks.h"
#include "indicators.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace csv_history {

using StatusHandler = std::function<void(const std::string&)>;
using LogHandler = std::function<void(const std::string&)>;
using HistorySeedHandler =
    std::function<void(const candlesticks::Candlestick&, const indicators::IndicatorSnapshot&)>;

std::optional<int64_t> lastStoredCsvOpenTimeMs(const std::filesystem::path& csvPath);

void replayCsvIntoEngine(const std::filesystem::path& csvPath, indicators::IndicatorEngine& engine,
                         const HistorySeedHandler& seedHistory,
                         const std::atomic<bool>* cancel = nullptr);

}  // namespace csv_history
