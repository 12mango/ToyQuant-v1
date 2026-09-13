#pragma once
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

#include "common/types.h"
#include "exchange/order.h"
#include "market/market_event.h"

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
    virtual void on_bbo(const BboQuote& quote) = 0;
    virtual TopOfBook top(const std::string& symbol) = 0;
};

class OrderBook : public IOrderBook
{
   public:
    explicit OrderBook(double tick_size = PRICE_TICK_SIZE) : tick_size_(tick_size) {}

    struct OrderNode
    {
        exchange::Order order;
        OrderState state{OrderState::New};
    };

    void add_order(const exchange::Order& order);
    bool cancel_order(uint64_t order_id);
    // filled_qty is the quantity filled by this execution.
    bool apply_partial_fill(uint64_t order_id, uint64_t filled_qty);
    OrderState state_for_order(uint64_t order_id) const;
    void on_tick(const Tick& t) override;
    void on_bbo(const BboQuote& quote) override;
    TopOfBook top(const std::string& symbol) override;

   private:
    mutable std::mutex mtx_;
    struct SideBook
    {
        std::map<PriceTick, uint64_t, std::greater<PriceTick>> bids_qty;
        std::map<PriceTick, uint64_t> asks_qty;
        std::map<PriceTick, std::list<OrderNode>, std::greater<PriceTick>> bids_orders;
        std::map<PriceTick, std::list<OrderNode>> asks_orders;
        BboQuote external_bbo;
        bool has_external_bbo{false};
    };
    double tick_size_;
    std::map<std::string, SideBook> books_;
    std::unordered_map<uint64_t, OrderNode*> order_index_;
    std::unordered_map<uint64_t, OrderState> state_index_;

    static uint64_t level_quantity(const std::list<OrderNode>& orders);
};