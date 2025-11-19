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
#include <chrono>

static const std::string DEFAULT_TICK_FILE   = "data/synthetic_ticks.csv";
static const std::string DEFAULT_ORDERS_FILE = "data/orders.csv";
static const std::string DEFAULT_TRADES_FILE = "data/trades.csv";
static const int DEFAULT_DELAY = 0;
static const int DEFAULT_UDP_PORT = 9000;
static const int UDP_MAX_WAIT_MS = 5000; // 超时时间 5 秒

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

    if(mode == "udp" && path_or_port.empty()) path_or_port = std::to_string(DEFAULT_UDP_PORT);

    std::ofstream orders_out(orders_file);
    std::ofstream trades_out(trades_file);
    orders_out << "ts,symbol,side,price,quantity,order_id\n";
    trades_out << "ts,symbol,side,price,quantity,order_id\n";

    OrderBook ob;
    MarketMaker strat(100, 0.00001);
    MatchingEngine engine;
    std::atomic<uint64_t> next_order_id{1};

    engine.set_report_callback([&strat,&trades_out](const ExecutionReport& report){
        strat.on_order_update(report);
        if(report.exec_type == ExecType::Trade){
            trades_out << report.ts << ","
                       << report.symbol << ","
                       << (report.side == exchange::Side::Buy ? "B" : "S") << ","
                       << report.price << ","
                       << report.quantity << ","
                       << report.order_id << "\n";
        }
    });

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
            ex_order.ts = t.ts;
            ex_order.owner = "MarketMaker";

            orders_out << t.ts << ","
                       << o.symbol << ","
                       << (o.side == Order::BUY ? "B" : "S") << ","
                       << o.price << ","
                       << o.quantity << ","
                       << o.order_id << "\n";

            engine.send_order(ex_order);
        }
    };

    if(mode == "csv") {
        CsvFeed feed(path_or_port, cb, delay);
        feed.run();
    } 
if(mode == "udp") {
    int port = std::stoi(path_or_port);
    UdpFeed feed(port, cb);
    if(!feed.start()){
        std::cerr << "udp start failed\n";
        return 1;
    }
    std::cout << "udp listening on " << port << std::endl;

    const int total_ticks = 10000; // synthetic_ticks.csv 总行数
    auto start_time = std::chrono::steady_clock::now();
    while(feed.get_tick_count() < total_ticks){
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = std::chrono::steady_clock::now();
        if(std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count() > UDP_MAX_WAIT_MS){
            std::cout << "UDP feed timeout, exiting.\n";
            break;
        }
    }
}
    else {
        std::cout << "usage: ./hft_demo csv <path> [ms_delay]   or   ./hft_demo udp <port>\n";
    }

    std::cout << "Orders saved to: " << orders_file << "\n";
    std::cout << "Trades saved to: " << trades_file << "\n";
    return 0;
}
