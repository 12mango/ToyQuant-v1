cmake_minimum_required(VERSION 3.16)
project(hft_demo LANGUAGES CXX)

# ==================== 编译选项配置 ====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /O2)
else()
    # -pthread 由下面的 Threads::Threads 自动引入，此处移除冗余
    add_compile_options(-Wall -Wextra -O2 -march=native)
endif()

# ==================== 项目路径与预定义宏 ====================
# 将项目根目录绝对路径直接注入到 C++ 代码中，实现一劳永逸的路径锁定
add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# 自动建好 logs 目录
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/logs")

# ==================== 包含路径与公共源文件 ====================
set(COMMON_INCLUDE_DIRS
    src
    src/market
    src/orderbook
    src/strategy
    src/exchange
    src/backtest
    src/utils
)

# 抽取公共编译目标文件
set(COMMON_SOURCES
    src/market/csv_feed.cpp
    src/orderbook/orderbook.cpp
    src/exchange/matching_engine.cpp
)

# 依赖库（跨平台线程库）
find_package(Threads REQUIRED)

# ==================== 1. hft_demo 可执行文件 ====================
add_executable(hft_demo src/main.cpp src/market/udp_feed.cpp ${COMMON_SOURCES})
target_include_directories(hft_demo PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(hft_demo PRIVATE Threads::Threads)
set_target_properties(hft_demo PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== 2. backtest_main 可执行文件 ====================
add_executable(backtest_main src/backtest/backtest_main.cpp src/backtest/backtest_driver.cpp ${COMMON_SOURCES})
target_include_directories(backtest_main PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(backtest_main PRIVATE Threads::Threads)
set_target_properties(backtest_main PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ==================== 3. 策略状态机测试 ====================
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