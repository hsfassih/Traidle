cmake_minimum_required(VERSION 3.16)
project(traidle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(traidle
    src/binance.cpp
    src/candlesticks.cpp
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