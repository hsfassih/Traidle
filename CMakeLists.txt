cmake_minimum_required(VERSION 3.16)
project(traidle CXX)

if(MINGW)
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -lstdc++ -lgcc_s -lgcc")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(traidle
    src/main.cpp
    src/utils/pipeline_common.cpp
    src/binance/binance.cpp
    src/binance/historical_get.cpp
    src/utils/candlesticks.cpp
    src/utils/candle_builder.cpp
    src/utils/csv_history.cpp
    src/capital/capital_api.cpp
    src/capital/capital_history.cpp
    src/capital/capital.cpp
    src/utils/indicators.cpp
    src/session_manager.cpp
    src/visualization.cpp
    src/smc-ict/ny_time.cpp
    src/smc-ict/smc_thread_pool.cpp
    src/smc-ict/swings.cpp
    src/smc-ict/fair_value_gaps.cpp
    src/smc-ict/break_of_structure.cpp
    src/smc-ict/change_of_character.cpp
    src/smc-ict/order_blocks.cpp
    src/smc-ict/liquidity.cpp
    src/smc-ict/previous_levels.cpp
    src/smc-ict/kill_zones.cpp
    src/smc-ict/premium_discount.cpp
    src/smc-ict/smc_engine.cpp
)

target_link_libraries(traidle PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
)

if(WIN32)
    target_link_libraries(traidle PRIVATE ws2_32 mswsock crypt32)
endif()
# ---------------------------------------------------------------------------
# Optional developer targets (OFF by default; never built by the Docker image,
# whose build only runs `--target traidle`).
# ---------------------------------------------------------------------------
option(TRAIDLE_BUILD_TESTS "Build the standalone tests" OFF)
if(TRAIDLE_BUILD_TESTS)
    add_executable(candle_builder_test
        tests/candle_builder_test.cpp
        src/utils/candle_builder.cpp
    )
    target_link_libraries(candle_builder_test PRIVATE nlohmann_json::nlohmann_json)

    add_executable(csv_history_test
        tests/csv_history_test.cpp
        src/utils/csv_history.cpp
        src/utils/indicators.cpp
        src/utils/candlesticks.cpp
    )
    target_link_libraries(csv_history_test PRIVATE nlohmann_json::nlohmann_json)

    # Offline end-to-end test of the Capital.com path: a mock Capital server (TLS REST +
    # WebSocket) plus the real Capital client code. The Binance adapter and main() are
    # not compiled in. See tests/capital_mock_test.cpp. POSIX only (it redirects
    # stdout/stderr with dup2 to prove no secret is ever printed).
    if(NOT WIN32)
        add_executable(capital_mock_test
            tests/capital_mock_test.cpp
            src/utils/pipeline_common.cpp
            src/utils/candlesticks.cpp
            src/utils/candle_builder.cpp
            src/utils/csv_history.cpp
            src/capital/capital_api.cpp
            src/capital/capital_history.cpp
            src/capital/capital.cpp
            src/utils/indicators.cpp
            src/visualization.cpp
            src/smc-ict/ny_time.cpp
            src/smc-ict/smc_thread_pool.cpp
            src/smc-ict/swings.cpp
            src/smc-ict/fair_value_gaps.cpp
            src/smc-ict/break_of_structure.cpp
            src/smc-ict/change_of_character.cpp
            src/smc-ict/order_blocks.cpp
            src/smc-ict/liquidity.cpp
            src/smc-ict/previous_levels.cpp
            src/smc-ict/kill_zones.cpp
            src/smc-ict/premium_discount.cpp
            src/smc-ict/smc_engine.cpp
        )
        target_link_libraries(capital_mock_test PRIVATE
            Boost::system
            OpenSSL::SSL
            OpenSSL::Crypto
            nlohmann_json::nlohmann_json
            Threads::Threads
        )

        # Offline test of the browser-driven market picker / symbol switching: the real
        # VisualizationServer and SessionManager with FAKE market backends, driven by a real
        # WebSocket client that plays the browser (start, Disconnect, bad input, foreign
        # origins, rapid cycles). `session_manager_test --serve [port]` keeps the fake-backed
        # app running so web/visualizer.html can be tried without any exchange or credentials.
        # No Binance/Capital adapter is compiled in. See tests/session_manager_test.cpp.
        add_executable(session_manager_test
            tests/session_manager_test.cpp
            src/session_manager.cpp
            src/utils/pipeline_common.cpp
            src/utils/candlesticks.cpp
            src/utils/candle_builder.cpp
            src/capital/capital_api.cpp
            src/utils/indicators.cpp
            src/visualization.cpp
            src/smc-ict/ny_time.cpp
            src/smc-ict/smc_thread_pool.cpp
            src/smc-ict/swings.cpp
            src/smc-ict/fair_value_gaps.cpp
            src/smc-ict/break_of_structure.cpp
            src/smc-ict/change_of_character.cpp
            src/smc-ict/order_blocks.cpp
            src/smc-ict/liquidity.cpp
            src/smc-ict/previous_levels.cpp
            src/smc-ict/kill_zones.cpp
            src/smc-ict/premium_discount.cpp
            src/smc-ict/smc_engine.cpp
        )
        target_link_libraries(session_manager_test PRIVATE
            Boost::system
            OpenSSL::SSL
            OpenSSL::Crypto
            nlohmann_json::nlohmann_json
            Threads::Threads
        )
    endif()
endif()