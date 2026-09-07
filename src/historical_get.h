#pragma once

#include "indicators.h"

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

// Two separate callbacks, matching TerminalBoard's two display modes:
//   - status: frequent, transient progress ("backfilling: 129,000 so far")
//     that should overwrite the same row in place, exactly like a FORMING
//     candle update. Called often (per page while a big backfill runs), so
//     it must be cheap and must never grow the terminal's scrollback.
//   - log: rare, noteworthy events (the final backfill result, or a hard
//     failure) that are worth a permanent scrolling line. Called at most a
//     couple of times per timeframe for an entire backfill.
// Routing routine progress through `log` instead of `status` is exactly
// what causes the terminal to fall apart on a large backfill: hundreds of
// scroll lines break the fixed board's relative cursor-movement math once
// the console has to scroll past its buffer.
using StatusHandler = std::function<void(const std::string&)>;
using LogHandler = std::function<void(const std::string&)>;

// Backfills csvPath for `symbol`/`binanceInterval` until it is caught up to
// "now". Returns true once caught up (including the trivial case where
// nothing needed fetching, which does zero network requests and calls
// neither callback). Returns false only on an unrecoverable error (already
// reported via `log`); the caller can still fall through to the live path
// in that case, just with whatever gap remains unfilled. Safe to call from
// multiple threads concurrently (one per timeframe) - a shared rate
// limiter keeps combined request weight under Binance's per-IP budget.
//
// `engine` is the caller's IndicatorEngine for this exact symbol/timeframe
// (one instance per timeframe worker, matching the isolation binance.cpp
// already gives every other piece of per-timeframe state). Before doing
// anything else, this function replays every data row already present in
// csvPath through `engine`, oldest to newest, so its rolling windows,
// seeded EMAs, VWAP daily accumulator, and OBV running total are restored
// to wherever they'd be had the process never restarted - otherwise every
// restart would silently reset every indicator's warm-up to zero even with
// years of history already on disk. It then keeps feeding `engine` every
// candle it backfills, in order, so indicator values written for backfilled
// rows are correct and continuous with what was already stored. By the
// time this function returns (success or failure), `engine` reflects every
// row now on disk and is ready to be reused, unmodified, by the live
// streaming path - constructing a fresh IndicatorEngine for the live path
// instead would silently restart every indicator's warm-up.
bool ensureContinuousHistory(boost::asio::io_context& ioc, boost::asio::ssl::context& ctx,
                              const std::string& symbol, const std::string& binanceInterval,
                              const std::filesystem::path& csvPath, const StatusHandler& status,
                              const LogHandler& log, indicators::IndicatorEngine& engine);

}  // namespace historical