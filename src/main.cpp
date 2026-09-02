#include <immintrin.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

namespace
{
struct AppConfig
{
    std::string mode = "csv";
    std::string path_or_port = "data/synthetic_ticks.csv";
    int delay = 0;
    std::string strategy_name = "optimized";
};

std::string to_abs_path(const std::string& input_path)
{
    namespace fs = std::filesystem;
    fs::path p(input_path);
    if (p.is_absolute()) return p.string();
    return (fs::path(PROJECT_ROOT_DIR) / p).string();
}

AppConfig parse_config(int argc, char** argv)
{
    AppConfig cfg;
    if (argc >= 2) cfg.mode = argv[1];
    if (argc >= 3) cfg.path_or_port = argv[2];
    if (argc >= 4) cfg.delay = std::stoi(argv[3]);
    if (argc >= 5) cfg.strategy_name = argv[4];
    return cfg;
}

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name)
{
    if (strategy_name == "naive")
    {
        return std::make_unique<NaiveMarketMaker>(100, 0.00001);
    }
    return std::make_unique<OptimizedMarketMaker>(100, 0.000003);
}

std::string side_to_csv(Side side)
{
    return side == Side::Buy ? "B" : side == Side::Sell ? "S" : "N";
}

exchange::Order to_exchange_order(const StrategyOrder& strategy_order, uint64_t ts,
                                  const std::string& owner)
{
    exchange::Order order{};
    order.id = strategy_order.order_id;
    order.symbol = strategy_order.symbol;
    order.side = strategy_order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell;
    order.type = exchange::OrderType::Limit;
    order.price = strategy_order.price;
    order.qty = strategy_order.quantity;
    order.remaining = strategy_order.quantity;
    order.ts = ts;
    order.owner = owner;
    return order;
}

void write_order_csv_row(std::ofstream& out, uint64_t ts, const StrategyOrder& order)
{
    out << ts << "," << order.symbol << "," << side_to_csv(order.side) << "," << order.price << ","
        << order.quantity << "," << order.order_id << "\n";
}

void write_trade_csv_row(std::ofstream& out, const ExecutionReport& report)
{
    if (report.exec_type != ExecType::Trade) return;
    out << report.ts << "," << report.symbol << ","
        << (report.side == exchange::Side::Buy ? "B" : "S") << "," << report.price << ","
        << report.quantity << "," << report.order_id << "\n";
}

void print_tick(const Tick& t, const TopOfBook& top)
{
    std::cout << "[TICK] " << t.symbol << " ts:" << t.ts << " price:" << t.price
              << " size:" << t.size << " side:" << to_char(t.side)
              << " | Top Bid: " << top.bid_price << "@" << top.bid_size
              << " | Top Ask: " << top.ask_price << "@" << top.ask_size << "\n";
}

class Pipeline
{
   public:
    Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
             Strategy& strategy, IMatchingEngine& engine)
        : orders_out_(orders_out),
          trades_out_(trades_out),
          order_book_(order_book),
          strategy_(strategy),
          engine_(engine)
    {
        engine_.set_report_callback(
            [this](const ExecutionReport& report)
            {
                strategy_.on_order_update(report);
                if (report.exec_type == ExecType::Trade)
                {
                    ++trade_reports_;
                    trade_report_quantity_ += report.quantity;
                }
                write_trade_csv_row(trades_out_, report);
            });
    }

    void process_tick(const Tick& tick, bool enable_print = true)
    {
        order_book_.on_tick(tick);
        auto top = order_book_.top(tick.symbol);
        if (enable_print) print_tick(tick, top);

        auto orders = strategy_.on_top_of_book(tick.symbol, top);

        for (uint64_t order_id : strategy_.cancel_requests())
        {
            ++cancel_requests_;
            engine_.cancel_order(order_id);
        }

        for (auto& order : orders)
        {
            order.order_id = next_order_id_++;
            auto ex_order = to_exchange_order(order, tick.ts, "MarketMaker");
            ++submitted_orders_;
            submitted_quantity_ += order.quantity;
            write_order_csv_row(orders_out_, tick.ts, order);
            strategy_.on_order_submitted(order);
            engine_.send_order(ex_order);
        }
    }

    void print_summary() const
    {
        std::cout << "[SUMMARY] submitted_orders=" << submitted_orders_
                  << " submitted_quantity=" << submitted_quantity_
                  << " cancel_requests=" << cancel_requests_ << " trade_reports=" << trade_reports_
                  << " trade_report_quantity=" << trade_report_quantity_
                  << " net_position=" << strategy_.net_position()
                  << " working_orders=" << strategy_.working_order_count() << "\n";
    }

   private:
    std::ofstream& orders_out_;
    std::ofstream& trades_out_;
    IOrderBook& order_book_;
    Strategy& strategy_;
    IMatchingEngine& engine_;
    std::atomic<uint64_t> next_order_id_{1};
    uint64_t submitted_orders_{0};
    uint64_t submitted_quantity_{0};
    uint64_t cancel_requests_{0};
    uint64_t trade_reports_{0};
    uint64_t trade_report_quantity_{0};
};

struct OutputFiles
{
    std::ofstream orders;
    std::ofstream trades;
};

OutputFiles open_output_files()
{
    std::string orders_file = to_abs_path("data/orders.csv");
    std::string trades_file = to_abs_path("data/trades.csv");
    std::filesystem::create_directories(std::filesystem::path(orders_file).parent_path());

    OutputFiles files{std::ofstream(orders_file), std::ofstream(trades_file)};
    files.orders << "ts,symbol,side,price,quantity,order_id\n";
    files.trades << "ts,symbol,side,price,quantity,order_id\n";
    return files;
}

void run_csv_mode(const AppConfig& cfg)
{
    std::string csv_file = to_abs_path(cfg.path_or_port);
    std::cout << "[Mode: CSV] Opening: " << csv_file << " (delay: " << cfg.delay
              << "ms) strategy=" << cfg.strategy_name << "\n";

    auto output_files = open_output_files();
    OrderBook order_book;
    auto strategy = make_strategy(cfg.strategy_name);
    MatchingEngine engine;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine);
    CsvFeed feed(csv_file, [&](const Tick& tick) { pipeline.process_tick(tick, true); }, cfg.delay);
    feed.run();
    pipeline.print_summary();
}

void run_udp_mode(const AppConfig& cfg)
{
    int port = std::stoi(cfg.path_or_port);
    std::cout << "[Mode: UDP] Listening on UDP port: " << port
              << "... strategy=" << cfg.strategy_name << "\n";

    auto output_files = open_output_files();
    OrderBook order_book;
    auto strategy = make_strategy(cfg.strategy_name);
    MatchingEngine engine;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine);
    UdpFeed feed(port);
    feed.start();

    Tick tick;
    while (true)
    {
        if (feed.pop_tick(tick))
        {
            pipeline.process_tick(tick, true);
        }
        else
        {
            _mm_pause();
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const AppConfig cfg = parse_config(argc, argv);

    if (cfg.mode == "csv")
    {
        run_csv_mode(cfg);
        return 0;
    }

    if (cfg.mode == "udp")
    {
        run_udp_mode(cfg);
        return 0;
    }

    std::cerr << "Unknown mode: " << cfg.mode << "\n";
    std::cerr << "Usage: ./hft_demo csv <path_to_csv> [ms_delay] [strategy]\n";
    std::cerr << "   or: ./hft_demo udp <port> [strategy]\n";
    std::cerr << "   strategy: optimized (default) | naive\n";
    return 1;
}