#include <immintrin.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "exchange/matching_engine.h"
#include "market/csv_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"

static const std::string DEFAULT_TICK_FILE = "data/synthetic_ticks.csv";
static const std::string DEFAULT_ORDERS_FILE = "data/orders.csv";
static const std::string DEFAULT_TRADES_FILE = "data/trades.csv";
static const int DEFAULT_DELAY = 0;
static const int DEFAULT_UDP_PORT = 9000;

// 优化 1：去掉 std::endl，改用 \n 避免频繁 Flush
void print_tick(const Tick& t, const TopOfBook& top)
{
    std::cout << "tick " << t.symbol << " " << t.ts << " " << t.price << " " << t.size << " "
              << t.side << " | top bid " << top.bid_price << "@" << top.bid_size << " | ask "
              << top.ask_price << "@" << top.ask_size << "\n";
}

int main(int argc, char** argv)
{
    std::string tick_file = DEFAULT_TICK_FILE;
    std::string orders_file = DEFAULT_ORDERS_FILE;
    std::string trades_file = DEFAULT_TRADES_FILE;

    std::string mode = "csv";
    std::string path_or_port = tick_file;
    int delay = DEFAULT_DELAY;

    if (argc >= 2)
    {
        mode = argv[1];
        if (argc >= 3) path_or_port = argv[2];
        if (argc >= 4) delay = std::stoi(argv[3]);
    }
    else
    {
        std::cout << "No arguments provided, defaulting to CSV file: " << path_or_port << "\n";
    }

    if (mode == "udp" && path_or_port.empty()) path_or_port = std::to_string(DEFAULT_UDP_PORT);

    std::ofstream orders_out(orders_file);
    std::ofstream trades_out(trades_file);
    orders_out << "ts,symbol,side,price,quantity,order_id\n";
    trades_out << "ts,symbol,side,price,quantity,order_id\n";

    OrderBook ob;
    MarketMaker strat(100, 0.00001);
    MatchingEngine engine;
    std::atomic<uint64_t> next_order_id{1};

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

    // 优化 2：抽离通用的订单处理 Lambda，减少重复代码
    auto process_tick_and_trade = [&](const Tick& t, bool enable_print = false)
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
            ex_order.side = (o.side == Order::BUY ? exchange::Side::Buy : exchange::Side::Sell);
            ex_order.type = exchange::OrderType::Limit;
            ex_order.price = o.price;
            ex_order.qty = o.quantity;
            ex_order.remaining = o.quantity;
            ex_order.ts = t.ts;
            ex_order.owner = "MarketMaker";

            orders_out << t.ts << "," << o.symbol << "," << (o.side == Order::BUY ? "B" : "S")
                       << "," << o.price << "," << o.quantity << "," << o.order_id << "\n";

            engine.send_order(ex_order);
        }
    };

    if (mode == "csv")
    {
        CsvFeed feed(path_or_port, [&](const Tick& t) { process_tick_and_trade(t, true); }, delay);
        feed.run();
    }
    else if (mode == "udp")
    {
        int port = std::stoi(path_or_port);
        UdpFeed feed(port);
        feed.start();

        std::atomic<uint64_t> processed_count{0};
        const uint64_t total_ticks = 200;
        Tick t;

        while (true)
        {
            uint64_t processed = processed_count.load(std::memory_order_acquire);
            size_t unread = feed.unread_count();
            if (processed >= total_ticks && unread == 0) break;

            bool did_process = false;
            while (feed.pop_tick(t))
            {
                did_process = true;
                process_tick_and_trade(t, false);
                processed_count.fetch_add(1, std::memory_order_release);
            }

            if (!did_process)
            {
                _mm_pause();  // 低延迟自旋 hint
            }
        }
        feed.stop();
    }
    else
    {
        std::cout << "usage: ./hft_demo csv <path> [ms_delay]   or   ./hft_demo udp <port>\n";
    }

    std::cout << "Orders saved to: " << orders_file << "\n";
    std::cout << "Trades saved to: " << trades_file << "\n";
    return 0;
}