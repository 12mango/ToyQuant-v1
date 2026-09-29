#!/usr/bin/env python3
"""Decompose level-quantity decreases in an incremental depth stream into trades and cancels.

The queue model decides how much of a displayed-size decrease is treated as cancellations ahead of a
resting order. Only the `prorata` model moves the queue from those decreases, and it does so from a
proportional rule with no free parameter. This tool measures what the data says about that rule.

For every level whose displayed quantity falls, the trades printed at that price on that side between
the level's previous update and this one are the part that really traded, so the remainder is what was
cancelled or amended. That ratio is the number a queue model has to be consistent with.

The attribution is an approximation and the report says so: the feed sends absolute quantities, so a
level that falls and is refilled between two reports is seen as one net change, and trades at that
price in that window can then exceed the decrease. The fraction of events where that happens is
printed, because it bounds the error.

Usage:
    python3 tools/calibrate_queue_model.py <incremental_depth.csv> <trades.csv[.gz]>
"""

import argparse
import bisect
import csv
import gzip
from collections import defaultdict
from pathlib import Path


def open_text(path):
    if str(path).endswith(".gz"):
        return gzip.open(path, "rt", newline="")
    return open(path, "r", newline="")


def load_trades(path):
    """Exchange-timestamp series of traded size per (side, price).

    A taker sell hits the bid and a taker buy lifts the ask, matching how the engine consumes the
    displayed size in process_market_trade.
    """
    traded = defaultdict(list)
    with open_text(path) as handle:
        for row in csv.DictReader(handle):
            side = "bid" if row["side"] == "sell" else "ask"
            key = (side, float(row["price"]))
            traded[key].append((int(row["timestamp"]), int(row["amount"])))
    for key in traded:
        traded[key].sort()
    return traded


def traded_in_window(series, start, end):
    if not series:
        return 0
    stamps = [entry[0] for entry in series]
    low = bisect.bisect_right(stamps, start)
    high = bisect.bisect_right(stamps, end)
    return sum(entry[1] for entry in series[low:high])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("depth", type=Path)
    parser.add_argument("trades", type=Path)
    parser.add_argument("--max-rows", type=int, default=0, help="0 reads the whole depth file")
    parser.add_argument("--top-levels", type=int, default=20)
    args = parser.parse_args()

    traded = load_trades(args.trades)

    quantity = {}
    last_ts = {}
    updates_per_level = defaultdict(int)
    per_level_cancel = defaultdict(int)
    per_level_decrease = defaultdict(int)

    total_decrease = 0
    decrease_events = 0
    traded_in_windows = 0
    over_traded_events = 0
    per_event_cancel_share = []

    rows = 0
    with open_text(args.depth) as handle:
        for row in csv.DictReader(handle):
            rows += 1
            if args.max_rows and rows > args.max_rows:
                break
            key = (row["side"], float(row["price"]))
            amount = int(row["amount"])
            timestamp = int(row["timestamp"])

            previous = quantity.get(key, 0)
            if amount < previous:
                decrease = previous - amount
                real = traded_in_window(traded.get(key), last_ts.get(key, 0), timestamp)
                if real > decrease:
                    over_traded_events += 1
                    real = decrease
                total_decrease += decrease
                traded_in_windows += real
                decrease_events += 1
                per_event_cancel_share.append((decrease - real) / decrease)
                per_level_cancel[key] += decrease - real
                per_level_decrease[key] += decrease
                updates_per_level[key] += 1

            quantity[key] = amount
            last_ts[key] = timestamp

    if decrease_events == 0:
        print("no level decreases found")
        return

    cancelled = total_decrease - traded_in_windows
    print(f"depth rows read          {rows}")
    print(f"levels with a decrease   {len(per_level_decrease)}")
    print(f"decrease events          {decrease_events}")
    print(f"total decrease           {total_decrease}")
    print(f"  explained by trades    {traded_in_windows}  ({traded_in_windows / total_decrease:.1%})")
    print(f"  therefore cancelled    {cancelled}  ({cancelled / total_decrease:.1%})")
    print("events where the trades exceeded the decrease, so the attribution is approximate: "
          f"{over_traded_events}  ({over_traded_events / decrease_events:.1%})")

    buckets = [0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 1.01]
    counts = [0] * (len(buckets) - 1)
    for share in per_event_cancel_share:
        for index in range(len(buckets) - 1):
            if buckets[index] <= share < buckets[index + 1]:
                counts[index] += 1
                break
    print("cancel share per event")
    for index in range(len(buckets) - 1):
        print(f"  {buckets[index]:.2f}-{buckets[index + 1]:.2f}  {counts[index]:7d}  "
              f"{counts[index] / decrease_events:6.1%}")

    print(f"busiest levels, top {args.top_levels} by decrease events")
    print(f"  {'side':4} {'price':>9} {'updates':>8} {'decrease':>12} {'cancelled':>12} {'share':>7}")
    ranked = sorted(per_level_decrease, key=lambda level: -updates_per_level[level])
    for key in ranked[: args.top_levels]:
        decrease = per_level_decrease[key]
        share = per_level_cancel[key] / decrease if decrease else 0.0
        print(f"  {key[0]:4} {key[1]:>9.1f} {updates_per_level[key]:>8} {decrease:>12} "
              f"{per_level_cancel[key]:>12} {share:>6.1%}")


if __name__ == "__main__":
    main()
