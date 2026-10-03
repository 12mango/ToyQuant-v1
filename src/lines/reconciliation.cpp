#include "lines/reconciliation.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
int64_t magnitude(int64_t value)
{
    return value < 0 ? -value : value;
}
}  // namespace

std::string ReconciliationSummary::to_log_string() const
{
    return "symbols_checked=" + std::to_string(symbols_checked) +
           " quantity_breaks=" + std::to_string(quantity_breaks) +
           " max_quantity_break=" + std::to_string(max_quantity_break) +
           " net_quantity_break=" + std::to_string(net_quantity_break) +
           " gross_quantity_break=" + std::to_string(gross_quantity_break) +
           " report_count_difference=" + std::to_string(report_count_difference) +
           " fee_difference=" + std::to_string(fee_difference) +
           " fees_match=" + (fees_match ? "1" : "0") + " status=" + (broken() ? "break" : "ok");
}

ReconciliationSummary reconcile_positions(const BrokerStatement& statement, const Portfolio& books)
{
    ReconciliationSummary summary;
    const auto& positions = books.positions();

    // Both populations, because a symbol one account has never heard of is a disagreement rather than an
    // absence: the statement's symbols come from the fills, the portfolio's from the reports applied.
    std::unordered_map<std::string, int64_t> expected;
    std::unordered_map<std::string, uint64_t> traded;
    for (const auto& [symbol, entry] : statement.symbols)
    {
        expected.emplace(symbol, entry.net_quantity);
        traded.emplace(symbol, entry.filled_quantity);
    }
    for (const auto& [symbol, position] : positions)
    {
        expected.emplace(symbol, 0);
        traded.emplace(symbol, 0);
    }

    for (const auto& [symbol, statement_quantity] : expected)
    {
        ++summary.symbols_checked;
        const auto position = positions.find(symbol);
        const int64_t booked = position == positions.end() ? 0 : position->second.quantity;
        const int64_t difference = statement_quantity - booked;
        if (difference != 0)
        {
            ++summary.quantity_breaks;
            summary.max_quantity_break =
                std::max(summary.max_quantity_break, magnitude(difference));
            summary.net_quantity_break += difference;
        }
        // The gross is compared as well as the net: a fill lost on each side leaves the position exactly
        // where it was and is still two fills this strategy was never told about.
        const uint64_t booked_traded =
            position == positions.end() ? 0 : position->second.traded_quantity;
        const uint64_t statement_traded = traded.at(symbol);
        summary.gross_quantity_break += statement_traded > booked_traded
                                            ? statement_traded - booked_traded
                                            : booked_traded - statement_traded;
    }

    const PortfolioMetrics metrics = books.metrics();
    const uint64_t booked_fills = metrics.maker_trade_count + metrics.taker_trade_count;
    // The fills the strategy was sent against the fills it booked, so a pipeline that lost or duplicated
    // a report in transit is caught; the venue's own execution count is deliberately not the left side,
    // because a fill split into two prints is two reports from one execution.
    summary.report_count_difference = statement.forwarded_fills > booked_fills
                                          ? statement.forwarded_fills - booked_fills
                                          : booked_fills - statement.forwarded_fills;
    summary.fee_difference = statement.fees - metrics.fees_paid;
    // A split fill adds its parts in a different order from the one fill they came from, so the two sums
    // can differ in the last bit; a real break is a difference large next to the fees themselves.
    const double scale = std::max(1.0, std::abs(statement.fees));
    summary.fees_match = std::abs(summary.fee_difference) <= 1e-9 * scale;
    return summary;
}
