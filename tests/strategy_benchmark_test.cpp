#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "app/strategy_benchmark.h"

namespace
{
const StrategyBenchmarkResult& find_result(const std::vector<StrategyBenchmarkResult>& results,
                                           std::string_view name)
{
    const auto it = std::find_if(results.begin(), results.end(),
                                 [&](const auto& result) { return result.strategy_name == name; });
    assert(it != results.end());
    return *it;
}
}  // namespace

int main()
{
    const auto temp = std::filesystem::temp_directory_path();
    const auto trades_path = temp / "toy_quant_flow_signal_trades.csv";
    const auto quotes_path = temp / "toy_quant_flow_signal_quotes.csv";
    const auto timeline_path = temp / "toy_quant_flow_signal_timeline.csv";
    std::ofstream trades(trades_path);
    std::ofstream quotes(quotes_path);
    assert(trades && quotes);

    quotes << "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,"
              "transaction_time,event_time\n";
    uint64_t timestamp = 1;
    uint64_t trade_id = 1;
    uint64_t quote_id = 1;
    double price = 70000.0;
    for (const int direction : {-1, 1})
    {
        for (int cycle = 0; cycle < 60; ++cycle)
        {
            double bid = price - 0.1;
            double ask = price + 0.1;
            quotes << quote_id++ << ',' << bid << ",5," << ask << ",5," << timestamp << ','
                   << timestamp << '\n';
            ++timestamp;

            const bool buyer_is_maker = direction < 0;
            trades << trade_id << ',' << (direction < 0 ? bid : ask) << ",0.0001," << trade_id
                   << ',' << trade_id << ',' << timestamp << ','
                   << (buyer_is_maker ? "true" : "false") << ",true\n";
            ++trade_id;
            ++timestamp;

            price += static_cast<double>(direction) * 0.1;
            bid = price - 0.1;
            ask = price + 0.1;
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                quotes << quote_id++ << ',' << bid << ",5," << ask << ",5," << timestamp << ','
                       << timestamp << '\n';
                ++timestamp;
            }

            trades << trade_id << ',' << (direction < 0 ? bid : ask) << ",0.001," << trade_id
                   << ',' << trade_id << ',' << timestamp << ','
                   << (buyer_is_maker ? "true" : "false") << ",true\n";
            ++trade_id;
            ++timestamp;
        }
    }
    trades.close();
    quotes.close();

    const auto results = run_strategy_benchmark(trades_path.string(), quotes_path.string(),
                                                "BTCUSDT", 1000000, {}, 0.60, 2.5, 0.10,
                                                0.40, timeline_path.string());
    const auto& inventory = find_result(results, "inventory_aware_l1");
    const auto& flow = find_result(results, "flow_aware_l1");

    assert(flow.dislocated_trades == 0);
    assert(flow.trade_reports < inventory.trade_reports);
    assert(flow.fees_paid < inventory.fees_paid);
    assert(std::abs(flow.net_position) < std::abs(inventory.net_position));
    assert(flow.equity > inventory.equity);
    assert(inventory.markout_count > 0);
    assert(inventory.average_abs_inventory >= 0.0);
    assert(flow.captured_edge != 0.0);

    std::ifstream timeline(timeline_path);
    std::string header;
    std::string first_record;
    assert(timeline && std::getline(timeline, header) && std::getline(timeline, first_record));
    assert(header.find("strategy,hour,ts") == 0);
    assert(first_record.find("passive_l1,") == 0);
}