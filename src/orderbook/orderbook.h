#pragma once
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

#include "common/types.h"
#include "exchange/order.h"

struct TopOfBook
{
    double bid_price{0};
    uint64_t bid_size{0};
    double ask_price{0};
    uint64_t ask_size{0};
};

enum class OrderState : uint8_t
{
    New = 0,
    Active,
    PartialFilled,
    Filled,
    Cancelled,
    Rejected
};

class IOrderBook
{
   public:
    virtual ~IOrderBook() = default;
    virtual void on_tick(const Tick& t) = 0;
    virtual TopOfBook top(const std::string& symbol) = 0;
};

class OrderBook : public IOrderBook
{
   public:
    struct OrderNode
    {
        exchange::Order order;
        OrderState state{OrderState::New};
    };

    void add_order(const exchange::Order& order);
    bool cancel_order(uint64_t order_id);
    // filled_qty is the quantity filled by this execution.
    bool apply_partial_fill(uint64_t order_id, uint64_t filled_qty);
    void on_tick(const Tick& t) override;
    TopOfBook top(const std::string& symbol) override;

   private:
    std::mutex mtx_;
    struct SideBook
    {
        std::map<double, uint64_t, std::greater<double>> bids_qty;
        std::map<double, uint64_t> asks_qty;
        std::map<double, std::list<OrderNode>, std::greater<double>> bids_orders;
        std::map<double, std::list<OrderNode>> asks_orders;
    };
    std::map<std::string, SideBook> books_;
    std::unordered_map<uint64_t, OrderNode*> order_index_;

    static uint64_t level_quantity(const std::list<OrderNode>& orders);
};