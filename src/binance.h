#pragma once

#include "pipeline_common.h"
#include "visualization.h"

#include <filesystem>
#include <string>

using namespace std;

namespace binance {

// One fully isolated worker per timeframe: own io_context/ssl::context, own
// CSV file, own REST-priming/WS-reconnect/watchdog/REST-fallback state, and
// its own IndicatorEngine and SmcEngine. Blocks until the worker's io_context
// stops (in practice: for the life of the process), so callers run it on its
// own thread. Moved out of binance.cpp's anonymous namespace unchanged so
// main.cpp can start it; binance.cpp no longer contains main().
void runTimeframeStream(const string& symbol, pipeline::TimeframeTask task,
                        filesystem::path csvPath, pipeline::TerminalBoard& board,
                        visualization::VisualizationServer& vizServer);

}  // namespace binance