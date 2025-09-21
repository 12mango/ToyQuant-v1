cmake_minimum_required(VERSION 3.16)
project(hft_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -march=native -pthread")

add_executable(hft_demo
    src/main.cpp
    src/market/csv_feed.cpp
    src/market/udp_feed.cpp
    src/orderbook/orderbook.cpp
    src/exchange/matching_engine.cpp
)

target_include_directories(hft_demo PRIVATE
    src
    src/market
    src/orderbook
    src/stategy
    src/exchange
)
