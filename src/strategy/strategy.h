#pragma once
#include <string>
#include <vector>

#include "common/types.h"
#include "exchange/execution_report.h"
#include "orderbook/orderbook.h"

struct StrategyOrder
{
    Side side = Side::Unknown;
    std::string symbol;
    double price = 0.0;
    uint64_t quantity = 0;
    uint64_t order_id = 0;

    StrategyOrder() = default;

    StrategyOrder(Side s, const std::string& sym, double p, uint64_t qty, uint64_t id)
        : side(s), symbol(sym), price(p), quantity(qty), order_id(id)
    {
    }
};

class Strategy
{
   public:
    virtual ~Strategy() = default;

    // 收到最新盘口时调用，返回要下的订单
    virtual std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                                      const TopOfBook& tob) = 0;

    // 订单获得最终 ID 并发往撮合引擎前调用。
    virtual void on_order_submitted(const StrategyOrder& order) = 0;

    // 收到成交、撤单、挂单等回报时调用
    virtual void on_order_update(const ExecutionReport& rpt) = 0;
};
