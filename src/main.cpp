#include "market/csv_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"
#include "exchange/matching_engine.h"
#include <iostream>
#include <thread>
#include <string>
#include <atomic>
#include <fstream>

// === 默认路径设置（集中管理） ===
static const std::string DEFAULT_TICK_FILE   = "data/synthetic_ticks.csv";
static const std::string DEFAULT_ORDERS_FILE = "data/orders.csv";
static const std::string DEFAULT_TRADES_FILE = "data/trades.csv";
static const int DEFAULT_DELAY = 0;

void print_tick(const Tick& t, const TopOfBook& top) {
    std::cout << "tick " << t.symbol 
              << " " << t.ts
              << " " << t.price
              << " " << t.size
              << " " << t.side
              << " | top bid " << top.bid_price << "@" << top.bid_size
              << " | ask " << top.ask_price << "@" << top.ask_size
              << std::endl;
}

int main(int argc, char** argv) {
    // === 文件路径 ===
    std::string tick_file   = DEFAULT_TICK_FILE;
    std::string orders_file = DEFAULT_ORDERS_FILE;
    std::string trades_file = DEFAULT_TRADES_FILE;

    std::string mode;
    std::string path_or_port;
    int delay = DEFAULT_DELAY;

    if(argc >= 2) {
        mode = argv[1];
        if(argc >= 3) path_or_port = argv[2];
        if(argc >= 4) delay = std::stoi(argv[3]);
    } else {
        mode = "csv";
        path_or_port = tick_file;
        std::cout << "No arguments provided, defaulting to CSV file: " << path_or_port << std::endl;
    }

    // === 打开输出文件 ===
    std::ofstream orders_out(orders_file);
    std::ofstream trades_out(trades_file);
    orders_out << "ts,symbol,side,price,quantity,order_id\n";
    trades_out << "ts,symbol,side,price,quantity,order_id\n";

    OrderBook ob;
    MarketMaker strat(10, 0.0003);
    MatchingEngine engine;

    // 全局自增 order_id
    std::atomic<uint64_t> next_order_id{1};

    // 注册撮合回报回调，传给策略，并写入 trades.csv
    engine.set_report_callback([&strat,&trades_out](const ExecutionReport& report){
        strat.on_order_update(report);
        if(report.exec_type == ExecType::Trade){
            trades_out << report.ts << ","
                       << report.symbol << ","
                       << (report.side == exchange::Side::Buy ? "B" : "S") << ","  // 可根据需要记录实际方向
                       << report.price << ","
                       << report.quantity << ","
                       << report.order_id << "\n";
        }
    });

    // tick 回调函数
    auto cb = [&ob, &strat, &engine, &next_order_id, &orders_out](const Tick& t){
        ob.on_tick(t);
        auto top = ob.top(t.symbol);
        print_tick(t, top);

        auto orders = strat.on_top_of_book(t.symbol, top);

        for (auto& o : orders) {
            o.order_id = next_order_id++;

            exchange::Order ex_order;
            ex_order.id = o.order_id;
            ex_order.symbol = o.symbol;
            ex_order.side = (o.side == Order::BUY ? exchange::Side::Buy : exchange::Side::Sell);
            ex_order.type = exchange::OrderType::Limit;
            ex_order.price = o.price;
            ex_order.qty = o.quantity;
            ex_order.remaining = o.quantity;
            ex_order.ts = t.ts; // 使用 tick 时间戳
            ex_order.owner = "MarketMaker";

            // 写入 orders.csv
            orders_out << t.ts << ","
                       << o.symbol << ","
                       << (o.side == Order::BUY ? "B" : "S") << ","
                       << o.price << ","
                       << o.quantity << ","
                       << o.order_id << "\n";

            engine.send_order(ex_order);
        }
    };

    // 启动数据源
    if(mode == "csv") {
        CsvFeed feed(path_or_port, cb, delay);
        feed.run();
    } else if(mode == "udp") {
        int port = std::stoi(path_or_port);
        UdpFeed feed(port, cb);
        if(!feed.start()) {
            std::cerr << "udp start failed\n";
            return 1;
        }
        std::cout << "udp listening on " << port << std::endl;
        std::this_thread::sleep_for(std::chrono::hours(24));
    } else {
        std::cout << "usage: ./hft_demo csv <path> [ms_delay]   or   ./hft_demo udp <port>\n";
    }

    std::cout << "Orders saved to: " << orders_file << "\n";
    std::cout << "Trades saved to: " << trades_file << "\n";
    return 0;
}
