#!/usr/bin/env python3
"""What a book costs to run, as a function of capital, breadth, price and the broker's volume tier.

The fee work in `docs/PERFORMANCE_HISTORY.md` established two things that a strategy choice depends on and
that no single window can answer: the per-share terms charge a fixed number of dollars a share, so a cheap
share is expensive in basis points, and the minimum per order is charged per *order*, so breadth at a
personal account's capital is bounded by `0.35 / (capital / names)`. This tool turns both into a table
before anything is bought, backtested or traded.

The model is deliberately the whole schedule and nothing else. One round trip is two orders, each paying

    max(minimum per order, commission a share * shares) + clearing + audit + rebate credit

with the SEC fee on the sale only, and the half spread paid on each side. A maker's rebate is a credit and a
taker pays the spread twice; both are priced, so the same capital can be read as a quoter's or a factor
book's.

What it does not model: market impact, borrow, financing, taxes, and the fact that spreads widen exactly when
a strategy wants to trade. Those make the numbers worse, which is why the tool is useful as a *filter*: a
configuration that fails here cannot be saved by anything it leaves out.

Usage:
    python3 tools/capacity_table.py --capital 50000 --names 5,10,20,50 --price 332 --spread-ticks 1
    python3 tools/capacity_table.py --capital 250000 --names 20,100 --price 20 --spread-ticks 3 \\
        --commission 0.0020 --turns 12
    python3 tools/capacity_table.py --capital 50000 --contract-notional 340000 --contract-cost 2.23 \\
        --contract-margin 15000
    python3 tools/capacity_table.py --self-test
"""

import argparse
import math
import sys

# IBKR Pro Tiered for US shares, read 2026-11-02: 0.0035 a share up to 300,000 shares a month, 0.0020 to
# 3,000,000, 0.0015 to 20,000,000, 0.0010 to 100,000,000 and 0.0005 above that; 0.35 minimum an order; SEC
# 0.0000206 of the proceeds of a sale; clearing 0.00020 a share; the consolidated audit trail 0.000003.
SCHEDULE = {
    "commission": 0.0035,
    "min_order": 0.35,
    "clearing": 0.0002,
    "audit": 0.000003,
    "regulatory": 0.0000206,
    "rebate": 0.0,
}
TIER_RATES = {"1": 0.0035, "2": 0.0020, "3": 0.0015, "4": 0.0010, "5": 0.0005}


def round_trip_usd(capital, names, price, spread_ticks, tick, schedule):
    """The dollars one name's round trip costs at this capital, and the shares it trades."""
    notional = capital / names
    shares = max(1, int(notional / price))
    commission = max(schedule["min_order"], schedule["commission"] * shares)
    per_side = commission + schedule["clearing"] * shares + schedule["audit"] * shares
    per_side -= schedule["rebate"] * shares
    # Two sides of commission and per-share fees, the SEC fee on the sale, and the half spread twice.
    total = 2.0 * per_side + schedule["regulatory"] * notional + spread_ticks * tick * shares
    return total, shares


def floor_free_clip(schedule):
    """The clip at which the floor stops binding: an order pays the greater of the two terms."""
    if schedule["commission"] <= 0.0:
        return 0
    return math.ceil(schedule["min_order"] / schedule["commission"])


def self_test():
    """Hand arithmetic on two cells, because a table of money is worth what its arithmetic is."""
    schedule = dict(SCHEDULE)
    # 50,000 over 20 names is 2,500 a name: seven shares of a 332 dollar stock, which pays the 0.35 floor
    # rather than the 0.0245 the commission would be, plus clearing, audit, one cent of spread on seven
    # shares and the SEC fee on the sale.
    total, shares = round_trip_usd(50000, 20, 332.0, 1.0, 0.01, schedule)
    assert shares == 7, shares
    expected = 2.0 * (0.35 + 0.0002 * 7 + 0.000003 * 7) + 0.0000206 * 2500.0 + 0.01 * 7
    assert abs(total - expected) < 1e-12, (total, expected)
    assert abs(total / 2500.0 * 1e4 - 3.297368) < 1e-5, total / 2500.0 * 1e4

    # The floor stops binding at 0.35 / 0.0035 = 100 shares, which is 33,200 dollars of a 332 dollar stock:
    # twenty such names need 664,000 of capital before a single order pays its own commission.
    assert floor_free_clip(schedule) == 100
    assert floor_free_clip(dict(schedule, commission=0.0020)) == 175
    assert floor_free_clip(dict(schedule, commission=0.0015)) == 234

    # Ten times the capital is a smaller fee in basis points at the same breadth, because the floor is spread
    # over ten times the notional.
    small = table(50000, [20], 332.0, 1.0, 0.01, schedule, 12)[0]
    large = table(500000, [20], 332.0, 1.0, 0.01, schedule, 12)[0]
    assert large["bps"] < small["bps"], (small, large)
    assert small["annual_bps"] > large["annual_bps"]

    # A rebate is a credit: turning it on lowers the round trip.
    credited = dict(schedule, rebate=0.0013)
    with_credit, _ = round_trip_usd(50000, 20, 332.0, 1.0, 0.01, credited)
    assert with_credit < total, (with_credit, total)
    print("self-test OK: the floor, the clip it needs, the capital scaling and the rebate all match hand "
          "arithmetic")


def table(capital, names_list, price, spread_ticks, tick, schedule, turns):
    """One row a name count: what an order is, what a round trip costs, and what it costs in a year."""
    rows = []
    for names in names_list:
        total, shares = round_trip_usd(capital, names, price, spread_ticks, tick, schedule)
        notional = capital / names
        bps = total / notional * 1e4
        rows.append({"names": names, "notional": notional, "shares": shares,
                     "round_trip": total, "bps": bps, "annual_bps": bps * turns})
    return rows


def print_shares(capital, names_list, price, spread_ticks, tick, schedule, turns, ceiling):
    print(f"capital {capital:,.0f} USD, price {price:g}, spread {spread_ticks:g} ticks of {tick:g}, "
          f"commission {schedule['commission']:g} a share, floor {schedule['min_order']:g} an order, "
          f"{turns:g} round trips a year")
    print()
    print("| names | notional a name | shares an order | round trip USD | round trip bps | a year bps | under the ceiling |")
    print("|---:|---:|---:|---:|---:|---:|---|")
    for row in table(capital, names_list, price, spread_ticks, tick, schedule, turns):
        mark = "yes" if row["bps"] <= ceiling else "no"
        print(f"| {row['names']} | {row['notional']:,.0f} | {row['shares']} | {row['round_trip']:.4f} | "
              f"{row['bps']:.3f} | {row['annual_bps']:.1f} | {mark} |")
    clip = floor_free_clip(schedule)
    print()
    print(f"the floor stops binding at {clip} shares an order, which at {price:g} is {clip * price:,.0f} USD a "
          f"name: a book of N names needs that many dollars times N before any order pays its own commission")
    affordable = [row["names"] for row in
                  table(capital, sorted(set(names_list)), price, spread_ticks, tick, schedule, turns)
                  if row["bps"] <= ceiling]
    if affordable:
        print(f"the widest book under {ceiling:g} bps a round trip at this capital is {max(affordable)} names")
    else:
        print(f"no breadth in this list is under {ceiling:g} bps a round trip at this capital")


def print_contract(capital, notional, cost, margin):
    print()
    print(f"| contracts | notional | cost a side USD | cost bps a side | capital at margin {margin:,.0f} |")
    print("|---:|---:|---:|---:|---:|")
    for contracts in (1, 2, 5, 10):
        print(f"| {contracts} | {contracts * notional:,.0f} | {cost * contracts:.2f} | "
              f"{cost / notional * 1e4:.4f} | {margin * contracts:,.0f} |")
    print()
    print(f"a fixed notional unit charges the same basis points at every size, which is the whole difference from "
          f"a share: {cost / notional * 1e4:.4f} bps a side depends on the contract's notional and its fee, not on "
          f"how many are held")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--capital", type=float, default=50000.0)
    parser.add_argument("--names", default="5,10,20,50,100")
    parser.add_argument("--price", type=float, default=100.0)
    parser.add_argument("--spread-ticks", type=float, default=1.0)
    parser.add_argument("--tick", type=float, default=0.01)
    parser.add_argument("--turns", type=float, default=12.0,
                        help="round trips a name a year; 12 is a monthly rebalance")
    parser.add_argument("--commission", type=float, default=SCHEDULE["commission"])
    parser.add_argument("--tier", default="", choices=["", "1", "2", "3", "4", "5"],
                        help="a monthly volume tier, as a shortcut for --commission")
    parser.add_argument("--min-order", type=float, default=SCHEDULE["min_order"])
    parser.add_argument("--clearing", type=float, default=SCHEDULE["clearing"])
    parser.add_argument("--audit", type=float, default=SCHEDULE["audit"])
    parser.add_argument("--regulatory", type=float, default=SCHEDULE["regulatory"])
    parser.add_argument("--rebate", type=float, default=SCHEDULE["rebate"])
    parser.add_argument("--ceiling-bps", type=float, default=10.0,
                        help="the round trip cost a name has to beat to be worth including")
    parser.add_argument("--contract-notional", type=float, default=0.0,
                        help="a fixed notional unit such as a future: its notional and its cost a side")
    parser.add_argument("--contract-cost", type=float, default=0.0)
    parser.add_argument("--contract-margin", type=float, default=15000.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    schedule = dict(SCHEDULE)
    schedule["commission"] = TIER_RATES[args.tier] if args.tier else args.commission
    schedule["min_order"] = args.min_order
    schedule["clearing"] = args.clearing
    schedule["audit"] = args.audit
    schedule["regulatory"] = args.regulatory
    schedule["rebate"] = args.rebate
    names = [int(value) for value in args.names.split(",") if value.strip()]
    if not names or any(value <= 0 for value in names):
        raise SystemExit("--names wants positive integers separated by commas")
    print_shares(args.capital, names, args.price, args.spread_ticks, args.tick, schedule, args.turns,
                 args.ceiling_bps)
    if args.contract_notional > 0.0:
        if args.contract_cost <= 0.0:
            raise SystemExit("--contract-notional needs --contract-cost")
        print_contract(args.contract_notional, args.contract_cost, args.contract_margin)
    return 0


if __name__ == "__main__":
    sys.exit(main())

