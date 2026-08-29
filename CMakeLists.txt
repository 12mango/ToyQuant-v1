cmake_minimum_required(VERSION 3.16)
project(hft_demo LANGUAGES CXX)

# ==================== 编译选项配置 ====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译优化标志
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-Wall -Wextra -O2 -march=native -pthread)
endif()

# ==================== 项目路径与预定义宏 ====================
# 1. 获取项目根目录 (跨平台/跨 CMake 版本兼容)
set(PROJECT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# 2. 在项目根目录下自动创建 logs 文件夹 (确保 Git 部署/编译时目录天然存在)
file(MAKE_DIRECTORY "${PROJECT_ROOT_DIR}/logs")

# 3. 将默认数据目录作为宏注入到 C++ 编译选项中
add_compile_definitions(DEFAULT_DATA_DIR="${PROJECT_ROOT_DIR}/data")

# ==================== 包含路径配置 ====================
set(COMMON_INCLUDE_DIRS
    src
    src/market
    src/orderbook
    src/strategy
    src/exchange
    src/backtest
    src/utils
)

# ==================== 1. hft_demo 可执行文件 ====================
add_executable(hft_demo
    src/main.cpp
    src/market/csv_feed.cpp
    src/market/udp_feed.cpp
    src/orderbook/orderbook.cpp
    src/exchange/matching_engine.cpp
)

target_include_directories(hft_demo PRIVATE ${COMMON_INCLUDE_DIRS})

# 设置调试工作目录为项目根目录 (针对 Windows Native 调试器)
set_target_properties(hft_demo PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_ROOT_DIR}"
)

# ==================== 2. backtest_main 可执行文件 ====================
add_executable(backtest_main
    src/backtest/backtest_main.cpp
    src/backtest/backtest_driver.cpp
    src/market/csv_feed.cpp
    src/orderbook/orderbook.cpp
    src/exchange/matching_engine.cpp
)

target_include_directories(backtest_main PRIVATE ${COMMON_INCLUDE_DIRS})

# 设置调试工作目录为项目根目录 (针对 Windows Native 调试器)
set_target_properties(backtest_main PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_ROOT_DIR}"
)

# Linux / WSL 线程库链接
find_package(Threads REQUIRED)
target_link_libraries(hft_demo PRIVATE Threads::Threads)
target_link_libraries(backtest_main PRIVATE Threads::Threads)