#include "strategy.h"

class MarketMaker : public Strategy {
public:
    uint64_t order_size;
    double spread;

    MarketMaker(uint64_t size, double spd) : order_size(size), spread(spd) {}

    std::vector<Order> on_top_of_book(const std::string& symbol, const TopOfBook& tob) override {
        std::vector<Order> orders;

        if(tob.bid_price > 0) {
            orders.push_back({Order::BUY, symbol, tob.bid_price - spread, order_size});
        }

        if(tob.ask_price > 0) {
            orders.push_back({Order::SELL, symbol, tob.ask_price + spread, order_size});
        }

        return orders;
    }
};
