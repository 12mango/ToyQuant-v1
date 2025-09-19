#include "market/csv_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"
#include <iostream>
#include <thread>
#include <string>

// === 宏定义默认 CSV 文件 ===
#define DEFAULT_CSV "/home/biaoge/projects/hft-demo/data/sample_ticks.csv"
#define DEFAULT_DELAY 0

void print_tick(const Tick& t, const TopOfBook& top) {
    std::cout << "tick " << t.symbol 
              << " " << t.ts
              << " " << t.price
              << " " << t.size
              << " " << t.side
              << " | top bid " << top.bid_price << "@" << top.bid_size
              << " ask " << top.ask_price << "@" << top.ask_size
              << std::endl;
}

void print_orders(const std::vector<Order>& orders) {
    for(const auto& o : orders) {
        std::cout << "  strategy order: "
                  << (o.side == Order::BUY ? "BUY" : "SELL")
                  << " " << o.symbol
                  << " " << o.price
                  << "@" << o.quantity
                  << std::endl;
    }
}

int main(int argc, char** argv) {
    OrderBook ob;
    MarketMaker strat(10, 0.0001); // 10手，点差0.0001（适合EURUSD）

    auto cb = [&ob, &strat](const Tick& t){
        ob.on_tick(t);
        auto top = ob.top(t.symbol);

        // 输出 tick
        print_tick(t, top);

        // 策略下单
        auto orders = strat.on_top_of_book(t.symbol, top);
        print_orders(orders);
    };

    std::string mode;
    std::string path_or_port;
    int delay = DEFAULT_DELAY;

    if(argc >= 2) {
        mode = argv[1];
        if(argc >= 3) path_or_port = argv[2];
        if(argc >= 4) delay = std::stoi(argv[3]);
    } else {
        // 默认启动 CSV
        mode = "csv";
        path_or_port = DEFAULT_CSV;
        std::cout << "No arguments provided, defaulting to CSV file: " << path_or_port << std::endl;
    }

    if(mode == "csv") {
        CsvFeed feed(path_or_port, cb, delay);
        feed.run();
        return 0;
    } else if(mode == "udp") {
        int port = std::stoi(path_or_port);
        UdpFeed feed(port, cb);
        if(!feed.start()) {
            std::cerr << "udp start failed\n";
            return 1;
        }
        std::cout << "udp listening on " << port << std::endl;
        std::this_thread::sleep_for(std::chrono::hours(24));
        return 0;
    }

    std::cout << "usage: ./hft_demo csv <path> [ms_delay]   or   ./hft_demo udp <port>\n";
    return 0;
}
