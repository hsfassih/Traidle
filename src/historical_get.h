#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include <filesystem>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// historical_get: fills in whatever closed candles are missing from a
// timeframe's CSV so it becomes continuous from (now - 3 years) up to the
// candle that is currently forming.
//
// One function, ensureContinuousHistory(), covers all three situations
// described in the workflow, because they are really the same operation at
// different distances:
//   1. Brand-new symbol/timeframe (CSV doesn't exist yet)      -> backfills
//      the full 3-year window.
//   2. An earlier run was interrupted partway through backfill -> resumes
//      from whatever was already written.
//   3. The program was simply restarted after some downtime    -> fills the
//      short gap between the last stored candle and now.
// In every case the function just asks "what's the oldest missing candle?"
// and requests forward from there until it catches up with the present.
//
// This module does not touch candlesticks.h / candlesticks.cpp or
// fetchCurrentCandle() in binance.cpp. It reuses
// candlesticks::parseKlineResponse() (by wrapping each row of a historical
// page as its own single-row response) instead of duplicating validation
// logic, and it owns its own small REST client, CSV read/write helpers, and
// a process-wide rate limiter so it stays fully independent of the
// live-streaming code path in binance.cpp.
// ---------------------------------------------------------------------------
namespace historical {

using LogHandler = std::function<void(const std::string&)>;

// Backfills csvPath for `symbol`/`binanceInterval` until it is caught up to
// "now". Returns true once caught up (including the trivial case where
// nothing needed fetching, which does zero network requests). Returns false
// only on an unrecoverable error (already reported via `log`); the caller
// can still fall through to the live path in that case, just with whatever
// gap remains unfilled. Safe to call from multiple threads concurrently
// (one per timeframe) - a shared rate limiter keeps combined request weight
// under Binance's per-IP budget.
bool ensureContinuousHistory(boost::asio::io_context& ioc, boost::asio::ssl::context& ctx,
                              const std::string& symbol, const std::string& binanceInterval,
                              const std::filesystem::path& csvPath, const LogHandler& log);

}  // namespace historical
