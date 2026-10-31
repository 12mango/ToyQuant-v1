#!/usr/bin/env python3
"""Test one event signal on free funding data: does a funding-rate extreme predict the next eight hours?

The rule was registered before the numbers were seen, so it lives here unchanged rather than in whichever form fitted
the result: at each settlement, a z-score of the rate against the previous ninety days; a short when the z is high (the
longs are paying and a contrarian collects both the funding and the squeeze) and a long when it is low; an eight-hour
hold; and two taker crossings at four basis points each.

The gates are part of the tool for the same reason: mean net above three times the cost, a worst trade shallower than
half the mean, positive in two thirds of the calendar years, and 2024-2026 held back so the answer to "does it survive"
is not the answer to "does it fit".

Usage:
    python3 tools/funding_extreme_test.py --self-test
    python3 tools/funding_extreme_test.py --start 2020-01 --end 2026-09
"""
import argparse
import io
import statistics
import sys
import urllib.error
import urllib.request
import zipfile
from datetime import datetime, timezone

MONTHLY = "https://data.binance.vision/data/futures/um/monthly"
UA = {"User-Agent": "toyquant"}
COST_BPS = 8.0
Z_WINDOW = 270
LOOKAHEAD_MS = 8 * 3600 * 1000


def month_list(start, end):
    year, month = start
    out = []
    while (year, month) <= end:
        out.append((year, month))
        year, month = (year + 1, 1) if month == 12 else (year, month + 1)
    return out


def fetch_zip(url):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=180) as response:
            blob = response.read()
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            return archive.read(archive.namelist()[0]).decode()
    except (urllib.error.HTTPError, urllib.error.URLError, zipfile.BadZipFile):
        return None


def funding_series(months):
    out = []
    for year, month in months:
        text = fetch_zip(f"{MONTHLY}/fundingRate/BTCUSDT/BTCUSDT-fundingRate-{year}-{month:02d}.zip")
        if text is None:
            print(f"  funding {year}-{month:02d} missing", file=sys.stderr)
            continue
        for line in text.splitlines():
            fields = line.split(",")
            if fields[0][:1].isdigit() and len(fields) >= 3:
                # The rule is registered in eight-hour steps and every BTCUSDT row declares eight, so a series that
                # settles on another cadence stops the run rather than being measured against a hold it never had.
                if int(fields[1]) != 8:
                    raise SystemExit(f"settlement interval {fields[1]}h at {fields[0]}, the rule is eight")
                out.append((int(fields[0]), float(fields[2])))
    return out


def price_series(months):
    out = {}
    for year, month in months:
        text = fetch_zip(f"{MONTHLY}/klines/BTCUSDT/1h/BTCUSDT-1h-{year}-{month:02d}.zip")
        if text is None:
            print(f"  klines {year}-{month:02d} missing", file=sys.stderr)
            continue
        for line in text.splitlines():
            fields = line.split(",")
            if fields[0][:1].isdigit():
                out[int(fields[0])] = float(fields[4])
    return out


def z_score(history, rate):
    """The rate against the mean and spread of the settlements before it."""
    mu = statistics.mean(history)
    sigma = statistics.pstdev(history)
    return None if sigma <= 0.0 else (rate - mu) / sigma


def trade_return(z, rate, entry, exit_price):
    """Net basis points of the rule's position, signed so a high z means short."""
    move = (exit_price / entry - 1.0) * 1e4
    gross = (-move + rate * 1e4) if z > 0 else (move - rate * 1e4)
    return gross - COST_BPS


def build_trades(settlements, prices):
    trades = []
    for index in range(Z_WINDOW, len(settlements) - 1):
        stamp, rate = settlements[index]
        z = z_score([value for _, value in settlements[index - Z_WINDOW:index]], rate)
        if z is None:
            continue
        entry = prices.get(stamp)
        exit_price = prices.get(stamp + LOOKAHEAD_MS)
        if entry is None or exit_price is None:
            continue
        trades.append({"ts": stamp, "z": z, "net": trade_return(z, rate, entry, exit_price)})
    return trades


def self_test():
    """The arithmetic, checked on a series whose answer is known by hand."""
    # A window with no spread has no z-score at all, and the rule skips it rather than inventing one.
    assert z_score([0.0001] * 10, 0.0001) is None
    assert z_score([0.0001, 0.0003], 0.0003) > 0.0
    # A high z is a short: price falls 10 bps and the rate pays 1 bp, so the rule earns 11 before the 8 bps of cost.
    assert abs(trade_return(2.0, 0.0001, 100.0, 99.9) - (10.0 + 1.0 - COST_BPS)) < 1e-9
    # A low z is a long, and with a negative rate the short side pays, so the long collects the same 1 bp.
    assert abs(trade_return(-2.0, -0.0001, 100.0, 100.1) - (10.0 + 1.0 - COST_BPS)) < 1e-9
    print("self-test OK: the z-score and the signed return match hand arithmetic at both extremes")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start", default="2020-01", help="first month, as YYYY-MM")
    parser.add_argument("--end", default="2026-09", help="last month, as YYYY-MM")
    parser.add_argument("--thresholds", default="1.5,2.0,3.0")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0

    months = month_list(tuple(map(int, args.start.split("-"))), tuple(map(int, args.end.split("-"))))
    settlements = funding_series(months)
    prices = price_series(months)
    print(f"{len(settlements)} settlements, {len(prices)} hourly closes")
    trades = build_trades(settlements, prices)
    print(f"{len(trades)} settlements with a full window\n")

    for level in [float(value) for value in args.thresholds.split(",")]:
        picked = [row for row in trades if abs(row["z"]) >= level]
        if not picked:
            continue
        net = [row["net"] for row in picked]
        by_year = {}
        for row in picked:
            year = datetime.fromtimestamp(row["ts"] / 1000, tz=timezone.utc).year
            by_year.setdefault(year, []).append(row["net"])
        print(f"|z| >= {level}: n={len(picked):4d}  mean {statistics.mean(net):+7.2f} bps  "
              f"median {statistics.median(net):+7.2f}  worst {min(net):+8.1f}  "
              f"positive {sum(1 for v in net if v > 0)}/{len(net)}")
        print("   " + "  ".join(f"{year}:{statistics.mean(series):+.1f}"
                                for year, series in sorted(by_year.items())))

    picked = [row for row in trades if abs(row["z"]) >= 2.0]
    if picked:
        net = [row["net"] for row in picked]
        mean_net = statistics.mean(net)
        by_year = {}
        for row in picked:
            year = datetime.fromtimestamp(row["ts"] / 1000, tz=timezone.utc).year
            by_year.setdefault(year, []).append(row["net"])
        positive_years = sum(1 for series in by_year.values() if statistics.mean(series) > 0)
        held = [row for row in picked
                if datetime.fromtimestamp(row["ts"] / 1000, tz=timezone.utc).year >= 2024]
        print(f"\ngates at |z| >= 2: n={len(net)}, mean {mean_net:+.2f} bps, worst {min(net):+.1f}")
        print(f"  gate1 mean above three times cost ({3 * COST_BPS:.0f}): "
              f"{'pass' if mean_net > 3 * COST_BPS else 'fail'}")
        print(f"  gate2 worst shallower than half the mean: "
              f"{'pass' if abs(min(net)) < abs(mean_net) / 2 else 'fail'}")
        print(f"  gate3 positive years {positive_years}/{len(by_year)}: "
              f"{'pass' if positive_years >= len(by_year) * 2 / 3 else 'fail'}")
        if held:
            print(f"  held out 2024-2026: n={len(held)}, mean "
                  f"{statistics.mean(row['net'] for row in held):+.2f} bps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
