#!/usr/bin/env python3
"""The low turnover test: a fixed rule, pre-registered here, on the sixty equity windows already on disk.

The cost structure this project measured leaves one direction untested. Market making pays a fee on every requote and
a carry pays one on every entry, and both therefore lose to a cost charged per action. A position held across minutes
pays the schedule once per round trip instead, so the question is not whether a signal exists but whether it clears a
round trip: the spread crossed twice plus the whole schedule.

The rule, fixed before any window was read, with no parameter to tune:

* a one minute grid of the mid from each file's top of book, and a thirty minute simple moving average of it;
* when the mid is two ticks or more below the average, buy 100 shares; two ticks or more above, sell 100 short;
* leave when the mid crosses back through the average, or after sixty minutes, whichever comes first, one position at
  a time, and never re-enter inside the same minute;
* fills pay the touch: a buy lifts the ask and a sell hits the bid, on entry and on exit, and the schedule is charged
  per share both ways -- 0.0035 commission, 0.000203 clearing and audit, and 0.0000206 of the price on a sale, with
  the 0.35 floor per order reported but not binding at 100 shares.

What it measures is one number: net cents a share a round trip, after the spread and the whole schedule, and the gross
centres a share beside it so the cost the rule has to clear is visible rather than implied.

Usage:
    python3 tools/lowturnover_test.py            # every file under data/v2/screen/
"""

import csv
import glob
import statistics
import sys

TICK = 0.01
BAND_TICKS = 2.0
GRID_SECONDS = 60
SMA_MINUTES = 30
HOLD_MINUTES = 60
SHARES = 100.0
COMMISSION = 0.0035
CLEARING = 0.0002
AUDIT = 0.000003
REGULATORY = 0.0000206


def minutes_of(path):
    """The last touch seen in each minute."""
    buckets = {}
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                bid, ask = float(row["bid_px_00"]), float(row["ask_px_00"])
                when = int(row["ts_event"]) // (GRID_SECONDS * 1_000_000_000)
            except (TypeError, ValueError, KeyError):
                continue
            if bid > 0 and ask > bid:
                buckets[when] = (bid, ask)
    return [(k, buckets[k][0], buckets[k][1]) for k in sorted(buckets)]


def run(minutes):
    trades = []
    position, entry, entry_index = 0, 0.0, 0
    for index in range(SMA_MINUTES, len(minutes)):
        window = minutes[index - SMA_MINUTES:index]
        average = sum((b + a) / 2.0 for _, b, a in window) / len(window)
        _, bid, ask = minutes[index]
        mid = (bid + ask) / 2.0
        if position == 0:
            if mid <= average - BAND_TICKS * TICK:
                position, entry, entry_index = SHARES, ask, index
            elif mid >= average + BAND_TICKS * TICK:
                position, entry, entry_index = -SHARES, bid, index
        else:
            held = index - entry_index
            back = (position > 0 and mid >= average) or (position < 0 and mid <= average)
            if back or held >= HOLD_MINUTES:
                exit_price = bid if position > 0 else ask
                gross = (exit_price - entry) * position
                fees = SHARES * (2 * COMMISSION + 2 * CLEARING + 2 * AUDIT)
                fees += SHARES * REGULATORY * (exit_price if position > 0 else entry)
                trades.append({"gross": gross, "fees": fees, "net": gross - fees,
                               "held": held, "long": position > 0})
                position = 0
    return trades


def main():
    rows = []
    for path in sorted(glob.glob("data/v2/screen/*.csv")):
        symbol = path.split("/")[-1].split("_")[0]
        minutes = minutes_of(path)
        if len(minutes) < SMA_MINUTES + 5:
            continue
        trades = run(minutes)
        if not trades:
            continue
        net = sum(t["net"] for t in trades)
        rows.append({"symbol": symbol, "trades": len(trades), "net": net,
                     "per_trade_c": 100.0 * net / SHARES / len(trades),
                     "gross_c": 100.0 * sum(t["gross"] for t in trades) / SHARES / len(trades),
                     "win": 100.0 * sum(1 for t in trades if t["net"] > 0) / len(trades),
                     "mean_hold": statistics.mean(t["held"] for t in trades)})
    print("| symbol | round trips | mean hold min | gross cents a share | net cents a share | "
          "share of round trips net positive | window net USD at 100 shares |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for r in sorted(rows, key=lambda x: -x["per_trade_c"]):
        print(f"| {r['symbol']} | {r['trades']} | {r['mean_hold']:.0f} | {r['gross_c']:+.2f} | "
              f"{r['per_trade_c']:+.2f} | {r['win']:.0f}% | {r['net']:+.2f} |")
    if rows:
        nets = [r["per_trade_c"] for r in rows]
        print()
        print(f"symbols measured {len(rows)}, positive after costs "
              f"{sum(1 for r in rows if r['per_trade_c'] > 0)}")
        print(f"median net a share a round trip {statistics.median(nets):+.2f} cents, "
              f"best {max(nets):+.2f}, worst {min(nets):+.2f}")
        print(f"median gross {statistics.median([r['gross_c'] for r in rows]):+.2f} cents a share")
    return 0


if __name__ == "__main__":
    sys.exit(main())
