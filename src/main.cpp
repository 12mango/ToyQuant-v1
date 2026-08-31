#include <immintrin.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "exchange/matching_engine.h"
#include "market/csv_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

// 路径辅助函数：相对路径强行锚定在项目根目录下，避免路径偏置
std::string to_abs_path(const std::string& input_path)
{
    namespace fs = std::filesystem;
    fs::path p(input_path);
    if (p.is_absolute()) return p.string();
    return (fs::path(PROJECT_ROOT_DIR) / p).string();
}

void print_tick(const Tick& t, const TopOfBook& top)
{
    std::cout << "[TICK] " << t.symbol << " ts:" << t.ts << " price:" << t.price
              << " size:" << t.size << " side:" << to_char(t.side)
              << " | Top Bid: " << top.bid_price << "@" << top.bid_size
              << " | Top Ask: " << top.ask_price << "@" << top.ask_size << "\n";
}

int main(int argc, char** argv)
{
    std::string mode = "csv";
    std::string path_or_port = "data/synthetic_ticks.csv";
    int delay = 0;

    if (argc >= 2) mode = argv[1];
    if (argc >= 3) path_or_port = argv[2];
    if (argc >= 4) delay = std::stoi(argv[3]);

    std::string orders_file = to_abs_path("data/orders.csv");
    std::string trades_file = to_abs_path("data/trades.csv");

    // 确保数据文件父级目录存在
    std::filesystem::create_directories(std::filesystem::path(orders_file).parent_path());

    std::ofstream orders_out(orders_file);
    std::ofstream trades_out(trades_file);
    orders_out << "ts,symbol,side,price,quantity,order_id\n";
    trades_out << "ts,symbol,side,price,quantity,order_id\n";

    OrderBook ob;
    MarketMaker strat(100, 0.00001);
    MatchingEngine engine;
    std::atomic<uint64_t> next_order_id{1};

    // 设置撮合引擎成交/状态回报回调
    engine.set_report_callback(
        [&strat, &trades_out](const ExecutionReport& report)
        {
            strat.on_order_update(report);
            if (report.exec_type == ExecType::Trade)
            {
                trades_out << report.ts << "," << report.symbol << ","
                           << (report.side == exchange::Side::Buy ? "B" : "S") << ","
                           << report.price << "," << report.quantity << "," << report.order_id
                           << "\n";
            }
        });

    // 统一处理 Tick 逻辑（行情更新 -> 策略触发 -> 发单给撮合引擎）
    auto process_tick_and_trade = [&](const Tick& t, bool enable_print = true)
    {
        ob.on_tick(t);
        auto top = ob.top(t.symbol);
        if (enable_print) print_tick(t, top);

        auto orders = strat.on_top_of_book(t.symbol, top);
        for (auto& o : orders)
        {
            o.order_id = next_order_id++;

            exchange::Order ex_order;
            ex_order.id = o.order_id;
            ex_order.symbol = o.symbol;
            ex_order.side = (o.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell);
            ex_order.type = exchange::OrderType::Limit;
            ex_order.price = o.price;
            ex_order.qty = o.quantity;
            ex_order.remaining = o.quantity;
            ex_order.ts = t.ts;
            ex_order.owner = "MarketMaker";

            orders_out << t.ts << "," << o.symbol << "," << (o.side == Side::Buy ? "B" : "S") << ","
                       << o.price << "," << o.quantity << "," << o.order_id << "\n";

            engine.send_order(ex_order);
        }
    };

    // ==================== 1. CSV 模式 ====================
    if (mode == "csv")
    {
        std::string csv_file = to_abs_path(path_or_port);
        std::cout << "[Mode: CSV] Opening: " << csv_file << " (delay: " << delay << "ms)\n";
        CsvFeed feed(csv_file, [&](const Tick& t) { process_tick_and_trade(t, true); }, delay);
        feed.run();
    }
    // ==================== 2. UDP 模式 (对接 Python 脚本) ====================
    else if (mode == "udp")
    {
        int port = std::stoi(path_or_port);
        std::cout << "[Mode: UDP] Listening on UDP port: " << port << "...\n";

        UdpFeed feed(port);
        feed.start();

        Tick t;
        // 无限循环监听 UDP 接收队列，使用 _mm_pause 实现高频低延迟消费
        while (true)
        {
            if (feed.pop_tick(t))
            {
                process_tick_and_trade(t, true);
            }
            else
            {
                _mm_pause();  // x86/x64 高频自旋指令，降低 CPU 功耗并提高响应
            }
        }
        feed.stop();
    }
    else
    {
        std::cerr << "Unknown mode: " << mode << "\n";
        std::cerr << "Usage: ./hft_demo csv <path_to_csv> [ms_delay]\n";
        std::cerr << "   or: ./hft_demo udp <port>\n";
        return 1;
    }

    std::cout << "Orders saved to: " << orders_file << "\n";
    std::cout << "Trades saved to: " << trades_file << "\n";
    return 0;
}