cmake_minimum_required(VERSION 3.16)
project(toy_quant LANGUAGES CXX)

# ==================== Compiler Options ====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /O2)
else()
    # Threads::Threads below supplies -pthread; do not add it redundantly here.
    add_compile_options(-Wall -Wextra -O2 -march=native)
endif()

# ==================== Project Paths and Definitions ====================
# Inject the absolute project root into the C++ code for stable relative paths.
add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Create the logs directory during configuration.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/logs")

# ==================== Include Paths and Shared Sources ====================
set(COMMON_INCLUDE_DIRS
    src
    src/market
    src/orderbook
    src/strategy
    src/exchange
    src/backtest
    src/utils
)

# Sources shared by application targets.
set(COMMON_SOURCES
    src/market/csv_feed.cpp
    src/orderbook/orderbook.cpp
    src/exchange/matching_engine.cpp
)

# Cross-platform threading library.
find_package(Threads REQUIRED)

# ==================== toy_quant Executable ====================
add_executable(toy_quant src/main.cpp src/market/udp_feed.cpp ${COMMON_SOURCES})
target_include_directories(toy_quant PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(toy_quant PRIVATE Threads::Threads)
set_target_properties(toy_quant PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== backtest_main Executable ====================
add_executable(backtest_main src/backtest/backtest_main.cpp src/backtest/backtest_driver.cpp ${COMMON_SOURCES})
target_include_directories(backtest_main PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(backtest_main PRIVATE Threads::Threads)
set_target_properties(backtest_main PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== Tests ====================
enable_testing()
add_executable(strategy_state_test tests/strategy_state_test.cpp)
target_include_directories(strategy_state_test PRIVATE ${COMMON_INCLUDE_DIRS})
add_test(NAME strategy_state_test COMMAND strategy_state_test)

add_executable(orderbook_state_test tests/orderbook_state_test.cpp src/orderbook/orderbook.cpp)
target_include_directories(orderbook_state_test PRIVATE ${COMMON_INCLUDE_DIRS})
add_test(NAME orderbook_state_test COMMAND orderbook_state_test)

add_executable(matching_engine_test tests/matching_engine_test.cpp ${COMMON_SOURCES})
target_include_directories(matching_engine_test PRIVATE ${COMMON_INCLUDE_DIRS})
add_test(NAME matching_engine_test COMMAND matching_engine_test)