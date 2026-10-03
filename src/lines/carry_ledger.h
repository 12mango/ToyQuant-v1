#pragma once

#include <cstdint>
#include <string>

#include "common/instrument_spec.h"
#include "common/funding.h"

// What a two leg carry earned and what it paid, over the span two funding histories share.
//
// The units are USD. Each leg holds `notional_usd`, so `gross_notional_usd` is twice one leg and
// `net_notional_usd` is the difference between them, which is what the account is actually exposed to.
// Both are printed and neither is called "the position", because reading one leg's notional as the
// account's is exactly the mistake a hedge invites: it would make the book look twice its size, and a
// return computed against it half of what it is.
struct CarrySummary
{
    std::string leg_a_symbol{};
    std::string leg_b_symbol{};
    double notional_usd{0.0};          // held on each leg
    double gross_notional_usd{0.0};    // both legs, which is the size a hedge of this notional carries
    double net_notional_usd{0.0};      // the account's exposure after the hedge
    uint64_t window_hours{0};          // the span both funding histories cover
    uint64_t settlements{0};           // settlements applied across both legs inside that span
    double leg_a_funding_usd{0.0};     // signed, so a positive number is money received
    double leg_b_funding_usd{0.0};
    double funding_usd{0.0};
    double fees_usd{0.0};              // entry and exit on both legs, at the venues' taker rates
    double net_usd{0.0};               // funding minus fees, over the window
    double annualized_carry_usd{0.0};  // the funding of the window continued over a year
    double annualized_return{0.0};     // the same as a fraction of one leg's notional
    double to_fees{0.0};               // annualized carry divided by the fees: the acceptance number
    double breakeven_hours{0.0};       // how long the carry must run to pay the fees, 0 if it never does

    std::string to_log_string() const;
};

// One leg: the instrument whose symbol and fee schedule are used, its funding history, and its side.
struct CarryLeg
{
    InstrumentSpec instrument;
    FundingRateSeries rates;
    bool long_side{true};
};

struct CarryConfig
{
    CarryLeg leg_a;
    CarryLeg leg_b;
    double notional_usd{10000.0};
};

// Runs a two leg carry: the stated notional is held on each leg, one long and one short, from the first
// settlement the two histories share to the last one, and every settlement inside that span is charged to
// the leg it belongs to, at that leg's own mark. Both legs are opened and closed as taker fills at the
// venues' published rates, so the fees are the ones a position of this size actually pays.
//
// Takes the config by value because it consumes the two series' cursors: a caller can run the same carry
// twice and get the same answer, which is what makes the number quotable.
CarrySummary run_carry(CarryConfig config);
