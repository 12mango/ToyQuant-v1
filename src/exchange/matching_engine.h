#pragma once
#include <cctype>
#include <functional>
#include <list>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/types.h"
#include "execution_report.h"
#include "market/market_event.h"
#include "order.h"

// Every draw in QueueModel::Lumpy comes from a generator seeded with the date of the recorded window,
// so a lumpy run is reproducible by default. A different seed is a replication of the same model
// rather than a different model, which is why the number worth reporting is the spread across seeds.
inline constexpr uint64_t kDefaultQueueSeed = 20200401;

struct FeeSchedule
{
    double maker_rate{0.0};
    double taker_rate{0.0};
    uint64_t quantity_scale{1};
    // See InstrumentSpec::unit_notional_usd. 0 falls back to the traded price, which is correct
    // for spot style instruments. It must be set for contracts with a fixed USD face value,
    // otherwise the fee is charged on the base asset price instead of on the contract value.
    double unit_notional_usd{0.0};
};

struct PriceLevel
{
    std::list<exchange::Order> orders;
    uint64_t external_queue_ahead{0};
};

struct MEOrderBook
{
    std::map<PriceTick, PriceLevel, std::greater<PriceTick>> bids;  // Highest price first.
    std::map<PriceTick, PriceLevel> asks;                           // Lowest price first.
};

class IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;
    virtual ~IMatchingEngine() = default;

    virtual void send_order(const exchange::Order& order) = 0;
    virtual void process_bbo(const BboQuote& quote) = 0;
    virtual void process_l2_top(const BboQuote& quote)
    {
        process_bbo(quote);
    }
    virtual void process_market_trade(const MarketTrade& trade) = 0;
    virtual void cancel_order(uint64_t order_id) = 0;
    virtual void set_report_callback(ReportCallback cb) = 0;
    virtual uint64_t queue_ahead_consumed() const
    {
        return 0;
    }
    virtual uint64_t queue_ahead_consumed(Side side) const
    {
        (void)side;
        return 0;
    }

    virtual uint64_t queue_ahead_levels_cleared(Side side) const
    {
        (void)side;
        return 0;
    }
    virtual uint64_t queue_ahead_from_quantity_changes(Side side) const
    {
        (void)side;
        return 0;
    }
};

// The engine keys every per-symbol map by this id rather than by the symbol string. A run touches
// one or two symbols millions of times, and hashing a thirteen character symbol on each state
// touch costs more than the bookkeeping it guards: the L2 replay reaches about nine of these maps
// per incremental batch, all with the same symbol. The registry hands out an id the first time a
// symbol appears and keeps it for the life of the engine, so the ids the maps hold stay valid.
using SymbolId = uint32_t;

// Returned by find_symbol for a symbol the engine has never seen.
inline constexpr SymbolId kUnknownSymbol = 0;

class MatchingEngine : public IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;

    explicit MatchingEngine(double tick_size = PRICE_TICK_SIZE, FeeSchedule fee_schedule = {},
                             uint64_t cancel_delay_events = 0,
                             QueueModel queue_model = QueueModel::ProRata,
                             uint64_t lump_chunks = 1, uint64_t random_seed = kDefaultQueueSeed,
                             double arrival_share = 1.0, uint64_t order_latency_us = 0,
                             uint64_t cancel_latency_us = 0)
                : tick_size_(tick_size),
                    fee_schedule_(fee_schedule),
                    queue_model_(queue_model),
                    lump_chunks_(lump_chunks == 0 ? 1 : lump_chunks),
                    queue_rng_(random_seed),
                    arrival_share_(arrival_share < 0.0 ? 0.0
                                                       : arrival_share > 1.0 ? 1.0 : arrival_share),
                    order_latency_us_(order_latency_us),
                    cancel_latency_us_(cancel_latency_us),
                    cancel_delay_events_(cancel_delay_events)
    {
    }

    void send_order(const exchange::Order& order) override;
    void process_bbo(const BboQuote& quote) override;
    void process_l2_top(const BboQuote& quote) override;
    void process_market_trade(const MarketTrade& trade) override;
    void cancel_order(uint64_t order_id) override;

    uint64_t queue_ahead_consumed() const override
    {
        return queue_ahead_consumed_;
    }

    uint64_t queue_ahead_consumed(Side side) const override
    {
        return side == Side::Buy ? buy_queue_ahead_consumed_ : sell_queue_ahead_consumed_;
    }

    uint64_t queue_ahead_levels_cleared(Side side) const override
    {
        return side == Side::Buy ? buy_queue_ahead_cleared_ : sell_queue_ahead_cleared_;
    }

    uint64_t queue_ahead_from_quantity_changes(Side side) const override
    {
        return side == Side::Buy ? buy_queue_from_quantity_changes_
                                 : sell_queue_from_quantity_changes_;
    }

    void set_report_callback(ReportCallback cb) override
    {
        report_cb_ = std::move(cb);
    }

   private:
    static std::string normalize_owner(const std::string& owner)
    {
        std::string normalized;
        normalized.reserve(owner.size());
        for (unsigned char ch : owner)
        {
            if (!std::isspace(ch)) normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
        return normalized;
    }

    bool is_self_trade(const exchange::Order& resting, const exchange::Order& incoming) const
    {
        if (resting.owner.empty() || incoming.owner.empty()) return false;
        return normalize_owner(resting.owner) == normalize_owner(incoming.owner);
    }

    // A quote that fails this check invalidates the stored BBO for its symbol rather than replacing
    // it, so every entry point has to agree on it.
    static bool is_usable_quote(const BboQuote& quote)
    {
        return !quote.symbol.empty() && quote.bid_price > 0.0 && quote.ask_price > 0.0 &&
               quote.bid_price <= quote.ask_price;
    }

    // Returns the id for `symbol`, assigning one on first sight.
    SymbolId symbol_id(const std::string& symbol)
    {
        const auto [entry, inserted] = symbol_ids_.try_emplace(symbol, next_symbol_id_);
        if (inserted) ++next_symbol_id_;
        return entry->second;
    }

    // Returns the id for `symbol`, or kUnknownSymbol without registering it. Used where the symbol
    // is only being looked up, so a rejected event does not grow the registry.
    SymbolId find_symbol(const std::string& symbol) const
    {
        const auto entry = symbol_ids_.find(symbol);
        return entry == symbol_ids_.end() ? kUnknownSymbol : entry->second;
    }

    // The body of process_bbo, with the symbol already resolved.
    void process_bbo_for(SymbolId id, const BboQuote& quote);
    void match_external_bbo(SymbolId id, exchange::Order& order);
    void cancel_order_immediate(uint64_t order_id);
    void apply_pending_cancels();
    uint64_t displayed_quantity_ahead(const exchange::Order& order) const;
    void process_market_order(SymbolId id, const std::string& symbol, Side side, double price,
                              uint64_t quantity, uint64_t ts);
    void match(MEOrderBook& book, const exchange::Order& incoming, bool rest_incoming);
    void report_trade(const exchange::Order& order, double price, uint64_t quantity,
                      LiquidityRole liquidity_role, uint64_t ts);
    void report(const ExecutionReport& rpt)
    {
        if (report_cb_) report_cb_(rpt);
    }

    std::unordered_map<std::string, SymbolId> symbol_ids_;
    SymbolId next_symbol_id_{1};
    std::unordered_map<SymbolId, MEOrderBook> books_;
    std::unordered_map<SymbolId, BboQuote> external_bbo_;
    std::unordered_map<SymbolId, uint64_t> last_bid_trade_ts_;
    std::unordered_map<SymbolId, uint64_t> last_ask_trade_ts_;
    std::unordered_map<SymbolId, BboQuote> l2_top_bbo_;
    std::unordered_set<SymbolId> queue_ahead_ids_;
    std::unordered_map<uint64_t, exchange::Order*> order_index_;
    ReportCallback report_cb_;
    double tick_size_;
    FeeSchedule fee_schedule_;
    uint64_t queue_ahead_consumed_{0};
    uint64_t buy_queue_ahead_consumed_{0};
    uint64_t sell_queue_ahead_consumed_{0};
    uint64_t buy_queue_ahead_cleared_{0};
    uint64_t sell_queue_ahead_cleared_{0};
    uint64_t buy_queue_from_quantity_changes_{0};
    uint64_t sell_queue_from_quantity_changes_{0};
    // The share of a level's displayed size that a newly arriving order starts behind: alpha in
    // Q0 = alpha * D0, so q starts at alpha. 1.0 is what a FIFO venue does at the moment the order
    // joins, since a new order is always last, and 0.0 would mean starting at the front. It is a knob
    // because of where the arithmetic leaves the uncertainty: the amount a decrease removes from the
    // queue ahead is r * q, so at q = 1 the placement of the cancellations does not matter at all and
    // every model but the conservative one removes the whole decrease. The arrival share, not the queue
    // model, is therefore what sets the headline fill count, and the models only separate once q < 1.
    uint64_t scale_arrival(uint64_t displayed) const;
    // Delivers every order whose one-way latency has expired, in the order the exchange clock reaches
    // them. With zero latency there is nothing in flight and this costs one empty check per event.
    void advance_to(uint64_t ts);
    void send_order_now(const exchange::Order& order);

    QueueModel queue_model_{QueueModel::ProRata};
    // Chunks per level decrease for QueueModel::Lumpy, and the generator that draws them. The seed is
    // fixed so a lumpy run is reproducible: one seed is a replication, not a random experiment, and
    // the spread across seeds is the quantity worth reporting.
    uint64_t lump_chunks_{1};
    std::mt19937_64 queue_rng_{kDefaultQueueSeed};
    double arrival_share_{1.0};
    // Orders do not exist in the book until the exchange has had them for this long, measured on the
    // clock the market events carry. Zero is a synchronous engine, which is what every recorded run
    // uses; anything else is the one part of the order lifecycle that a replay can model directly, and
    // the reason the project can measure what being slow costs rather than only asserting that it does.
    uint64_t order_latency_us_{0};
    // The other half of the order lifecycle, and the half that costs a market maker money: a quote that
    // arrives late is merely stale, but a cancel that arrives late leaves the quote standing through
    // whatever the market did in the meantime. Cancels are due when both the event count and the clock
    // say so, so the original event-counted delay keeps working underneath this one.
    uint64_t cancel_latency_us_{0};
    // The last market event's timestamp, which is also the moment a strategy's decision was taken,
    // because a strategy only ever acts on a market event.
    uint64_t now_us_{0};
    std::vector<std::pair<uint64_t, exchange::Order>> in_flight_orders_;
    uint64_t event_counter_{0};
    struct PendingCancel
    {
        uint64_t due_event{0};
        uint64_t due_time{0};
    };
    std::unordered_map<uint64_t, PendingCancel> pending_cancels_;
    uint64_t cancel_delay_events_{0};
};
