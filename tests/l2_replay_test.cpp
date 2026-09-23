#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>

#include "market/l2_replay_feed.h"

int main()
{
    const auto root = std::filesystem::temp_directory_path();
    const auto trades_path = root / "toy_quant_l2_replay_trades.csv";
    const auto depth_path = root / "toy_quant_l2_replay_depth.csv";
    {
        std::ofstream trades(trades_path);
        trades << "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"
               << "deribit,BTC-PERPETUAL,100,101,10,buy,100.5,2\n"
               << "deribit,BTC-PERPETUAL,300,301,11,sell,100.0,3\n";
        std::ofstream depth(depth_path);
        depth << "exchange,symbol,timestamp,asks[0].price,asks[0].amount,bids[0].price,bids[0].amount\n"
              << "deribit,BTC-PERPETUAL,50,101.0,5,100.0,6\n"
              << "deribit,BTC-PERPETUAL,200,101.5,7,100.5,8\n";
    }

    std::vector<uint64_t> timestamps;
    L2ReplayFeed feed(
        make_deribit_trade_reader(trades_path.string()),
        make_deribit_book_snapshot_reader(depth_path.string()),
        [&timestamps](const MarketEvent& event)
        { timestamps.push_back(std::visit([](const auto& value) { return value.ts; }, event)); });
    feed.run();

    assert((timestamps == std::vector<uint64_t>{50, 100, 200, 300}));
    assert(feed.validation_summary().events == 4);
    assert(feed.validation_summary().trades == 2);
    assert(feed.validation_summary().depth_snapshots == 2);

    std::filesystem::remove(trades_path);
    std::filesystem::remove(depth_path);
}
