#pragma once

#include "../utils/pipeline_common.h"
#include "../utils/stop_signal.h"
#include "../visualization.h"

#include <filesystem>
#include <string>

using namespace std;

namespace binance {

// One fully isolated worker per timeframe: own io_context/ssl::context, own
// CSV file, own REST-priming/WS-reconnect/watchdog/REST-fallback state, and
// its own IndicatorEngine and SmcEngine. Blocks until the worker's io_context
// stops, so callers run it on its own thread.
//
// `stop`, if given, ends the worker: once stop->request() is called the history
// backfill is abandoned, the io_context is stopped, and this function returns
// (the browser's Disconnect button relies on this). Without it the worker runs
// for the life of the process, as before. The signal must outlive the call.
void runTimeframeStream(const string& symbol, pipeline::TimeframeTask task,
                        filesystem::path csvPath, pipeline::TerminalBoard& board,
                        visualization::VisualizationServer& vizServer,
                        app::StopSignal* stop = nullptr);

// One cheap REST call (the same /fapi/v1/klines request the live path uses) to
// catch a mistyped symbol before any worker starts. Returns false and fills
// `errorText` ONLY when Binance definitively rejects the symbol (HTTP 400);
// any other outcome - network trouble, geo-block, rate limit - returns true so a
// transient problem never refuses a valid symbol: the workers have their own
// retry/fallback logic for those.
bool checkSymbol(const string& symbol, string& errorText);

}  // namespace binance