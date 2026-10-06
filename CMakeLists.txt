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
    src/concurrency
    src/app
    src/market
    src/orderbook
    src/strategy
    src/exchange
    src/backtest
    src/utils
)

# Sources shared by application targets.
# The market data readers: one file per feed family, plus the router that picks between them. A new venue is
# a new file and a line here rather than another thousand line file, and every target that parses market data
# takes the whole set.
set(MARKET_READER_SOURCES
    src/market/market_data_adapter.cpp
    src/market/binance_readers.cpp
    src/market/deribit_readers.cpp
    src/market/okx_readers.cpp)

# The layer boundary, stated as two lists and checked by tools/check_layering.py on every gate run.
# `CORE_SOURCES` is what a reader learns the engine from: it carries no venue, no legacy header and no carry ledger,
# so `common <- orderbook/strategy/exchange/accounting <- app` stays readable in that direction only.
# `LINE_SOURCES` is everything that belongs to one feed family or to a closed line. It stays in the build because the
# frozen tag and the checked documents need it re-derivable, and it is listed separately so that the boundary is a
# fact a tool can read rather than a convention a reader has to trust.
set(CORE_SOURCES
    src/accounting/portfolio.cpp
    src/backtest/performance.cpp
    src/exchange/matching_engine.cpp
    src/market/l2_replay_feed.cpp
    src/market/line_reader.cpp
    src/market/market_data_validator.cpp
    src/market/market_event_recorder.cpp
    src/market/replay_feed.cpp
    src/market/threaded_l2_replay.cpp
    src/orderbook/l2_orderbook.cpp
    src/orderbook/tick_ladder.cpp
    src/utils/logger.cpp
    src/utils/stage_profiler.cpp)

set(LINE_SOURCES
    src/lines/reconciliation.cpp
    src/lines/carry_ledger.cpp
    src/lines/paper_broker.cpp
    src/lines/legacy/tick_csv_parser.cpp
    src/lines/legacy/order_book.cpp
    src/market/csv_feed.cpp
    src/market/udp_feed.cpp
    src/market/funding_rate.cpp
    ${MARKET_READER_SOURCES})

# The application layer's own files, listed here for the same reason as the two above: a target takes the
# layer rather than repeating what is in it.
set(APP_SOURCES
    src/app/order_ledger.cpp
    src/app/pipeline.cpp
    src/app/run_accounting.cpp
    src/app/strategy_benchmark.cpp
    src/app/strategy_factory.cpp)

# Cross-platform threading library.
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# ==================== Layer Libraries ====================
# Each list above is compiled once, into one library, and a target takes the layers it needs. The
# duplication this removes was real but not the part that mattered: `src/market/line_reader.cpp` was
# written out in eight targets, and a source missing from one of them is a link error at the end of a build
# rather than a compile error at the file, which is exactly how the legacy book's move failed the first
# time. A clean build went from two hundred and forty-one translation units to the number of distinct files
# the targets actually use.
#
# Static rather than object libraries, because a static archive is pulled object by object: a test of the
# ladder links the ladder and not the engine, while naming all three layers is a statement about which
# layers a target may reach into rather than about what ends up in its binary. The edges between the layers
# are declared so the link order is a consequence of the graph rather than of the order they are named in.

add_library(toy_core STATIC ${CORE_SOURCES})
add_library(toy_lines STATIC ${LINE_SOURCES})
add_library(toy_app STATIC ${APP_SOURCES})
target_include_directories(toy_core PUBLIC ${COMMON_INCLUDE_DIRS})
target_include_directories(toy_lines PUBLIC ${COMMON_INCLUDE_DIRS})
target_include_directories(toy_app PUBLIC ${COMMON_INCLUDE_DIRS})
target_link_libraries(toy_core PUBLIC Threads::Threads ZLIB::ZLIB)
target_link_libraries(toy_lines PUBLIC toy_core Threads::Threads ZLIB::ZLIB)
target_link_libraries(toy_app PUBLIC toy_core toy_lines Threads::Threads ZLIB::ZLIB)

# The profiling build cannot take those objects, which is worth writing down where the objects are defined:
# `-pg` has to be on the file that is profiled, so linking the shared objects would instrument `main.cpp`
# and nothing else, producing a profile of an empty program. It takes the same three variables instead,
# which keeps the lists in one place even for the target that cannot share the results.
set(STACK_SOURCES ${CORE_SOURCES} ${LINE_SOURCES} ${APP_SOURCES})

# ==================== toy_quant Executable ====================
# The entry point and the two files only it uses. Everything else arrives through the three layers.
add_executable(toy_quant src/main.cpp src/app/app_config.cpp src/app/application.cpp)
target_link_libraries(toy_quant PRIVATE toy_core toy_lines toy_app)
set_target_properties(toy_quant PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT MSVC)
    add_executable(toy_quant_profile EXCLUDE_FROM_ALL src/main.cpp src/app/app_config.cpp
        src/app/application.cpp)
    target_sources(toy_quant_profile PRIVATE ${STACK_SOURCES})
    target_include_directories(toy_quant_profile PRIVATE ${COMMON_INCLUDE_DIRS})
    target_link_libraries(toy_quant_profile PRIVATE Threads::Threads ZLIB::ZLIB)
    target_compile_options(toy_quant_profile PRIVATE -pg)
    target_link_options(toy_quant_profile PRIVATE -pg)
    set_target_properties(toy_quant_profile PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# ==================== backtest_main Executable ====================
add_executable(backtest_main src/backtest/backtest_main.cpp src/backtest/backtest_driver.cpp)
target_link_libraries(backtest_main PRIVATE toy_core toy_lines toy_app)
set_target_properties(backtest_main PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== Strategy benchmark ====================
add_executable(strategy_benchmark src/app/strategy_benchmark_main.cpp)
target_link_libraries(strategy_benchmark PRIVATE toy_core toy_lines toy_app)
set_target_properties(strategy_benchmark PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== Ring hand-off benchmark ====================
# What one message through the ring costs, against the two alternatives a reader would otherwise reach
# for. Built by the normal presets so it cannot rot, and deliberately not registered with ctest: a timing
# number is not an assertion. The gate reads its deterministic `messages` and `checksum` fields instead.
add_executable(ring_bench src/bench/ring_bench_main.cpp)
target_include_directories(ring_bench PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(ring_bench PRIVATE Threads::Threads)
set_target_properties(ring_bench PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== Tests ====================
enable_testing()

# Every test makes the same three decisions -- one file, the whole stack, and checked in every build -- so
# they are made once here instead of twenty-one times, and the second one is the reason the function exists.
# `assert` is the test: `linux-relwithdebinfo` defines NDEBUG, and a test compiled with NDEBUG is a program
# that exits zero while checking nothing, which is why twenty-one files used to carry their own `#undef`.
# A flag cannot be forgotten when a test is added; a boilerplate header can. `-UNDEBUG` is spelled `/U NDEBUG`
# with a space for MSVC, which would otherwise read it as `/U NTDEBUG`.
function(add_toy_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE toy_core toy_lines toy_app)
    if(MSVC)
        target_compile_options(${name} PRIVATE "/U NDEBUG")
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_toy_test(strategy_state_test)
add_toy_test(orderbook_state_test)
add_toy_test(tick_ladder_test)
add_toy_test(l2_orderbook_test)
add_toy_test(l2_strategy_test)
add_toy_test(l2_replay_test)
add_toy_test(market_event_recorder_test)
add_toy_test(spsc_ring_test)
add_toy_test(threaded_replay_test)

add_toy_test(paper_broker_test)
add_toy_test(matching_engine_test)
add_toy_test(tick_csv_parser_test)
add_toy_test(line_reader_test)
add_toy_test(stage_profiler_test)
add_toy_test(market_data_adapter_test)
add_toy_test(performance_test)
add_toy_test(portfolio_test)

add_toy_test(carry_ledger_test)
add_toy_test(funding_rate_test)
add_toy_test(pipeline_accounting_test)
add_toy_test(strategy_benchmark_test)