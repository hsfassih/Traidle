cmake_minimum_required(VERSION 3.16)
project(strategy_lint CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(strategy_lint main.cpp expr.cpp checks_strategy.cpp checks_example.cpp)
target_link_libraries(strategy_lint PRIVATE nlohmann_json::nlohmann_json)

