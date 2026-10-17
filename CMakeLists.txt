cmake_minimum_required(VERSION 3.16)
project(traidle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(traidle
    src/main.cpp
    src/pipeline_common.cpp
    src/binance.cpp
    src/candlesticks.cpp
    src/candle_builder.cpp
    src/capital_api.cpp
    src/capital_history.cpp
    src/capital.cpp
    src/historical_get.cpp
    src/indicators.cpp
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
option(TRAIDLE_BUILD_TESTS "Build the standalone tests (candle_builder_test, capital_mock_test)" OFF)
if(TRAIDLE_BUILD_TESTS)
    add_executable(candle_builder_test
        tests/candle_builder_test.cpp
        src/candle_builder.cpp
    )
    target_link_libraries(candle_builder_test PRIVATE nlohmann_json::nlohmann_json)

    # Offline end-to-end test of the Capital.com path: a mock Capital server (TLS REST +
    # WebSocket) plus the real client code. Everything in traidle except main() and the
    # Binance adapter is compiled in. See tests/capital_mock_test.cpp. POSIX only (it
    # redirects stdout/stderr with dup2 to prove no secret is ever printed).
    if(NOT WIN32)
        add_executable(capital_mock_test
            tests/capital_mock_test.cpp
            src/pipeline_common.cpp
            src/candlesticks.cpp
            src/candle_builder.cpp
            src/capital_api.cpp
            src/capital_history.cpp
            src/capital.cpp
            src/historical_get.cpp
            src/indicators.cpp
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
    endif()
endif()