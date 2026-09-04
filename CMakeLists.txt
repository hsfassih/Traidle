cmake_minimum_required(VERSION 3.16)
project(traidle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(traidle
    src/binance.cpp
    src/candlesticks.cpp
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

add_executable(ws_diagnostic
    scripts/ws_diagnostic.cpp
)

target_link_libraries(ws_diagnostic PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(ws_diagnostic PRIVATE ws2_32 mswsock crypt32)
endif()