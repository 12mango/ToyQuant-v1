#include "market/csv_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"
#include "exchange/matching_engine.h"
#include <iostream>
#include <thread>
#include <string>
#include <atomic>

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
              << " | ask " << top.ask_price << "@" << top.ask_size
              << std::endl;
}

int main(int argc, char** argv) {
    OrderBook ob;
    MarketMaker strat(10, 0.0001); // 10手，点差0.0001（适合EURUSD）
    MatchingEngine engine;

    // 全局自增 order_id
    std::atomic<uint64_t> next_order_id{1};

    // 注册撮合回报回调，传给策略
    engine.set_report_callback([&strat](const ExecutionReport& report){
        strat.on_order_update(report);
    });

    // tick 回调函数
auto cb = [&ob, &strat, &engine, &next_order_id](const Tick& t){
    // 1. 更新行情
    ob.on_tick(t);
    auto top = ob.top(t.symbol);
    print_tick(t, top);

    // 2. 策略生成订单
    auto orders = strat.on_top_of_book(t.symbol, top);

    // 3. 为策略订单分配 order_id 并转换成撮合引擎订单
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
        ex_order.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                             ).count();
        ex_order.owner = "MarketMaker";

        engine.send_order(ex_order);
    }
};

    // 解析启动参数
    std::string mode;
    std::string path_or_port;
    int delay = DEFAULT_DELAY;

    if(argc >= 2) {
        mode = argv[1];
        if(argc >= 3) path_or_port = argv[2];
        if(argc >= 4) delay = std::stoi(argv[3]);
    } else {
        mode = "csv";
        path_or_port = DEFAULT_CSV;
        std::cout << "No arguments provided, defaulting to CSV file: " << path_or_port << std::endl;
    }

    // 启动数据源
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
