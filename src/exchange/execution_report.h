#pragma once
#include <cstdint>
#include <string>

#include "common/types.h"
#include "order.h"

enum class LiquidityRole : uint8_t
{
    Unknown = 0,
    Maker,
    Taker
};

// Why an order ended without trading. `None` is every report that is not a rejection. The distinction
// matters to a strategy: a venue refusal, a broker refusal and a client-side timeout all mean "the order
// is not working", but only the first two are a refusal of the order itself.
enum class RejectReason : uint8_t
{
    None = 0,
    Venue,
    Broker,
    Timeout,
    Risk
};

inline const char* reject_reason_name(RejectReason reason)
{
    switch (reason)
    {
        case RejectReason::Venue:
            return "venue";
        case RejectReason::Broker:
            return "broker";
        case RejectReason::Timeout:
            return "timeout";
        case RejectReason::Risk:
            return "risk";
        case RejectReason::None:
            break;
    }
    return "none";
}

// The reports that end an order's life. Every place that releases pending quantity, closes an audit
// entry or erases a strategy's open order uses this one predicate, so a new terminal type cannot be
// added to one path and forgotten in another.
inline bool is_terminal(ExecType type)
{
    return type == ExecType::Filled || type == ExecType::Cancelled || type == ExecType::Rejected;
}

// Why an order was cancelled. `None` is every cancel whose source predates this field or does not know.
// The distinction matters to a retry policy: a user cancel is expected and can be re-quoted immediately,
// while a venue cancel or a self-trade prevention is the venue telling the strategy that its state or its
// order flow is wrong, which deserves the same backoff as a rejection.
enum class CancelReason : uint8_t
{
    None = 0,
    User,
    Venue,
    Ioc,
    Fok,
    SelfTrade,
    Timeout
};

inline const char* cancel_reason_name(CancelReason reason)
{
    switch (reason)
    {
        case CancelReason::User:
            return "user";
        case CancelReason::Venue:
            return "venue";
        case CancelReason::Ioc:
            return "ioc";
        case CancelReason::Fok:
            return "fok";
        case CancelReason::SelfTrade:
            return "self_trade";
        case CancelReason::Timeout:
            return "timeout";
        case CancelReason::None:
            break;
    }
    return "none";
}

struct ExecutionReport
{
    uint64_t order_id{};
    exchange::Side side{};
    ExecType exec_type{};
    std::string symbol;
    double price{};
    // Trade reports this execution's fill quantity. PartialFill, Filled, Resting, and
    // Cancelled report the order's remaining quantity.
    uint64_t quantity{};
    uint64_t ts{};
    std::string owner;
    LiquidityRole liquidity_role{LiquidityRole::Unknown};
    double fee{0.0};
    // Only meaningful when exec_type is Rejected. Kept at the end so every existing aggregate
    // initializer, which names the fields in order, keeps compiling unchanged.
    RejectReason reject_reason{RejectReason::None};
    // Only meaningful when exec_type is Cancelled.
    CancelReason cancel_reason{CancelReason::None};

    uint64_t executed_quantity() const
    {
        return exec_type == ExecType::Trade ? quantity : 0;
    }

    uint64_t remaining_quantity() const
    {
        return exec_type == ExecType::Trade ? 0 : quantity;
    }
};
