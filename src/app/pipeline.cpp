#include "pipeline.h"

#include <cmath>
#include <type_traits>

#include "backtest/performance.h"
#include "exchange/execution_report.h"
#include "utils/logger.h"

namespace
{
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
    if (report.exec_type != ExecType::Trade || report.owner != "MarketMaker") return;
    const char* role = report.liquidity_role == LiquidityRole::Maker   ? "maker"
                       : report.liquidity_role == LiquidityRole::Taker ? "taker"
                                                                       : "unknown";
    out << report.ts << "," << report.symbol << ","
        << (report.side == exchange::Side::Buy ? "B" : "S") << "," << report.price << ","
        << report.quantity << "," << report.order_id << "," << role << "," << report.fee << "\n";
}

}  // namespace

Pipeline::Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
                   Strategy& strategy, IMatchingEngine& engine, Portfolio& portfolio,
                   Logger& logger, double tick_size)
    : orders_out_(orders_out),
      trades_out_(trades_out),
      order_book_(order_book),
      strategy_(strategy),
      engine_(engine),
      portfolio_(portfolio),
    logger_(logger),
    tick_size_(tick_size)
{
    engine_.set_report_callback(
        [this](const ExecutionReport& report)
        {
            if (report.owner == "MarketMaker") portfolio_.apply(report);
            if (report.owner == "MarketMaker" && report.exec_type == ExecType::Trade)
            {
                position_ += report.side == exchange::Side::Buy
                                 ? static_cast<int64_t>(report.executed_quantity())
                                 : -static_cast<int64_t>(report.executed_quantity());
                execution_quality_.captured_edge +=
                    report.side == exchange::Side::Buy ? last_mid_ - report.price
                                                       : report.price - last_mid_;
                pending_markouts_.push_back(
                    {report.side == exchange::Side::Buy ? Side::Buy : Side::Sell, report.price,
                     quote_cycle_});
            }
            if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
            {
                const auto start = order_start_cycles_.find(report.order_id);
                if (start != order_start_cycles_.end())
                {
                    const uint64_t lifetime = quote_cycle_ >= start->second
                                                  ? quote_cycle_ - start->second
                                                  : 0;
                    execution_quality_.total_quote_lifetime += lifetime;
                    execution_quality_.max_quote_lifetime =
                        std::max(execution_quality_.max_quote_lifetime, lifetime);
                    order_start_cycles_.erase(start);
                }
            }
            strategy_.on_order_update(report);
            if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
                pending_cancel_orders_.erase(report.order_id);
            auto audit = order_audit_.find(report.order_id);
            if (audit != order_audit_.end())
            {
                if (report.exec_type == ExecType::Trade) audit->second.filled = true;
                if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
                {
                    const uint64_t lifetime = quote_cycle_ >= audit->second.start_cycle
                                                  ? quote_cycle_ - audit->second.start_cycle
                                                  : 0;
                    execution_quality_.total_order_lifetime_cycles += lifetime;
                    if (report.exec_type == ExecType::Cancelled && audit->second.filled)
                    {
                        ++execution_quality_.cancelled_after_fill_orders;
                    }
                    else if (report.exec_type == ExecType::Cancelled)
                    {
                        ++execution_quality_.cancelled_before_fill_orders;
                    }
                    if (report.exec_type == ExecType::Filled)
                        ++execution_quality_.audited_filled_orders;
                    else
                        ++execution_quality_.audited_cancelled_orders;
                    order_audit_.erase(audit);
                }
            }
            if (report.exec_type == ExecType::Trade && report.owner == "MarketMaker")
            {
                ++trade_reports_;
                trade_report_quantity_ += report.executed_quantity();
            }
            write_trade_csv_row(trades_out_, report);
        });
}

void Pipeline::process_event(const MarketEvent& event, bool enable_print)
{
    const auto exchange = std::visit([](const auto& value) { return value.exchange; }, event);
    if (!exchange.empty())
    {
        if (market_exchange_.empty())
            market_exchange_ = exchange;
        else if (market_exchange_ != exchange)
            throw std::invalid_argument("market event exchange does not match pipeline source");
    }

    std::visit(
        [this, enable_print](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, MarketTrade>)
            {
                const auto quote_it = latest_quotes_.find(value.symbol);
                if (quote_it == latest_quotes_.end())
                {
                    ++filtered_market_trades_;
                    return;
                }
                const BboQuote& quote = quote_it->second;
                const double mid = (quote.bid_price + quote.ask_price) / 2.0;
                const double deviation_bps = std::abs(value.price - mid) / mid * 10000.0;
                if (deviation_bps > 5.0)
                {
                    ++filtered_market_trades_;
                    return;
                }
                strategy_.on_market_trade(value, &quote);
                engine_.process_market_trade(value);
            }
            else if constexpr (std::is_same_v<Event, BboQuote>)
            {
                latest_quotes_[value.symbol] = value;
                order_book_.on_bbo(value);
                engine_.process_bbo(value);
                const auto top = order_book_.market_top(value.symbol);
                last_mid_ = (value.bid_price + value.ask_price) / 2.0;
                ++quote_cycle_;
                while (!pending_markouts_.empty() &&
                       quote_cycle_ >= pending_markouts_.front().start_cycle + 5)
                {
                    const auto observation = pending_markouts_.front();
                    pending_markouts_.pop_front();
                    execution_quality_.adverse_selection +=
                        observation.side == Side::Buy ? observation.price - last_mid_
                                                      : last_mid_ - observation.price;
                    ++execution_quality_.markout_count;
                }
                const double abs_inventory = static_cast<double>(std::abs(position_));
                ++inventory_samples_;
                execution_quality_.average_abs_inventory +=
                    (abs_inventory - execution_quality_.average_abs_inventory) /
                    static_cast<double>(inventory_samples_);
                execution_quality_.max_abs_inventory =
                    std::max(execution_quality_.max_abs_inventory, std::abs(position_));
                process_top_of_book(value.symbol, value.ts, top, enable_print);
            }
            else
            {
                return;
            }
        },
        event);
}

void Pipeline::process_top_of_book(const std::string& symbol, uint64_t ts, const TopOfBook& top,
                                   bool enable_print)
{
    if (enable_print)
        logger_.debug("[TOP] symbol=", symbol, " ts=", ts, " bid=", top.bid_price, "@",
                      top.bid_size, " ask=", top.ask_price, "@", top.ask_size);
    submit_strategy_actions(symbol, ts, top);
}

void Pipeline::process_l2_market_view(const L2MarketView& view, bool enable_print)
{
    if (enable_print)
        logger_.debug("[L2] symbol=", view.symbol, " ts=", view.ts, " bid=",
                      view.top.bid_price, " ask=", view.top.ask_price,
                      " imbalance=", view.depth_imbalance, " micro=", view.micro_price);
    if (view.top.bid_price > 0.0 && view.top.ask_price > 0.0)
    {
        const BboQuote quote{.ts = view.ts,
                             .symbol = view.symbol,
                             .bid_price = view.top.bid_price,
                             .bid_quantity = view.top.bid_size,
                             .ask_price = view.top.ask_price,
                             .ask_quantity = view.top.ask_size,
                             .exchange = {}};
        latest_quotes_[view.symbol] = quote;
        engine_.process_l2_top(quote);
        last_mid_ = (view.top.bid_price + view.top.ask_price) / 2.0;
        ++quote_cycle_;
    }
    const uint64_t buy_queue = engine_.queue_ahead_consumed(Side::Buy);
    const uint64_t sell_queue = engine_.queue_ahead_consumed(Side::Sell);
    if (buy_queue > last_buy_queue_ahead_consumed_)
    {
        strategy_.on_queue_activity(Side::Buy, buy_queue - last_buy_queue_ahead_consumed_);
        last_buy_queue_ahead_consumed_ = buy_queue;
    }
    if (sell_queue > last_sell_queue_ahead_consumed_)
    {
        strategy_.on_queue_activity(Side::Sell, sell_queue - last_sell_queue_ahead_consumed_);
        last_sell_queue_ahead_consumed_ = sell_queue;
    }
    submit_strategy_actions(view.symbol, view.ts, strategy_.on_l2_market_view(view));
}

void Pipeline::process_l2_market_trade(const MarketTrade& trade)
{
    strategy_.on_market_trade(trade);
    engine_.process_market_trade(trade);
}

RunSummary Pipeline::summary() const
{
    return {.submitted_orders = submitted_orders_,
            .submitted_quantity = submitted_quantity_,
            .cancel_requests = cancel_requests_,
            .trade_reports = trade_reports_,
            .trade_report_quantity = trade_report_quantity_,
            .queue_ahead_consumed = engine_.queue_ahead_consumed(),
            .buy_queue_ahead_levels_cleared = engine_.queue_ahead_levels_cleared(Side::Buy),
            .sell_queue_ahead_levels_cleared = engine_.queue_ahead_levels_cleared(Side::Sell),
            .buy_queue_from_quantity_changes =
                engine_.queue_ahead_from_quantity_changes(Side::Buy),
            .sell_queue_from_quantity_changes =
                engine_.queue_ahead_from_quantity_changes(Side::Sell),
            .filtered_market_trades = filtered_market_trades_,
            .fill_rate = metrics::compute_fill_rate(submitted_quantity_, trade_report_quantity_),
            .cancel_rate = metrics::compute_cancel_rate(submitted_orders_, cancel_requests_),
            .working_orders = strategy_.working_order_count(),
            .portfolio = portfolio_.metrics(),
            .strategy = strategy_.metrics(),
            .execution_quality = execution_quality_};
}

void Pipeline::submit_strategy_actions(const std::string& symbol, uint64_t ts, const TopOfBook& top)
{
    submit_strategy_actions(symbol, ts, strategy_.on_top_of_book(symbol, top));
}

void Pipeline::submit_strategy_actions(const std::string& symbol, uint64_t ts,
                                       std::vector<StrategyOrder> orders)
{
    (void)symbol;

    for (uint64_t order_id : strategy_.cancel_requests())
    {
        if (!pending_cancel_orders_.insert(order_id).second) continue;
        ++cancel_requests_;
        engine_.cancel_order(order_id);
    }

    for (auto& order : orders)
    {
        order.order_id = next_order_id_++;
        auto exchange_order = to_exchange_order(order, ts, "MarketMaker");
        ++submitted_orders_;
        submitted_quantity_ += order.quantity;
        const auto quote_it = latest_quotes_.find(order.symbol);
        if (quote_it != latest_quotes_.end())
        {
            const BboQuote& quote = quote_it->second;
            const double reference = order.side == Side::Buy ? quote.bid_price : quote.ask_price;
            const double distance = order.side == Side::Buy ? reference - order.price
                                                             : order.price - reference;
            ++execution_quality_.quote_observations;
            execution_quality_.total_quote_distance += std::max(0.0, distance);
            execution_quality_.total_quote_distance_ticks +=
                std::max(0.0, distance) / std::max(tick_size_, PRICE_TICK_SIZE);
            if (distance <= 0.5 * std::max(tick_size_, PRICE_TICK_SIZE))
                ++execution_quality_.bbo_quote_observations;
        }
        write_order_csv_row(orders_out_, ts, order);
        strategy_.on_order_submitted(order);
        order_start_cycles_[order.order_id] = quote_cycle_;
        order_audit_[order.order_id] = OrderAudit{quote_cycle_, false};
        ++execution_quality_.audited_orders;
        engine_.send_order(exchange_order);
    }
}
