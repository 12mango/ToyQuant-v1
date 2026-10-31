#!/usr/bin/env python3
"""Fetch one at-the-money straddle per month from Deribit's free first-of-month samples, and invert its volatility.

The options section of the roadmap is the only measurement in this repository whose collection lived in a scratch
script, which made its numbers unreproducible in exactly the way the rest of the project refuses. This is that
collection as a tool, with the three decisions that cost a rewrite recorded where they are made:

* the strike is the nearest to spot on a 100-point grid, then 250, then 500, because the at-the-money straddle
  approximation that an earlier version used to read volatility off the price is only valid at the money;
* the implied volatility is inverted from Black-Scholes per leg, with a zero rate and a zero carry, which is what a
  coin-settled crypto option priced in its own coin amounts to;
* the expiry is the nearest listed one at least twenty days out, so every month contributes a sample rather than every
  quarter.

Usage:
    python3 tools/fetch_deribit_options.py --start 2020-01 --end 2026-09 --output data/v2/options/atm_straddles.csv
    python3 tools/fetch_deribit_options.py --self-test
"""

import argparse
import csv
import gzip
import io
import json
import math
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import date, datetime, timedelta, timezone

TARDIS = "https://datasets.tardis.dev/v1/deribit"
BINANCE = "https://data.binance.vision/data/spot/monthly/klines/BTCUSDT/1d"
NOON = 12
MONTHS = {1: "JAN", 2: "FEB", 3: "MAR", 4: "APR", 5: "MAY", 6: "JUN",
          7: "JUL", 8: "AUG", 9: "SEP", 10: "OCT", 11: "NOV", 12: "DEC"}
UA = {"User-Agent": "toyquant"}


def norm(value):
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def straddle_price(spot, strike, years, vol):
    """The price of a call plus a put at one strike, in quote currency, with a zero rate."""
    if years <= 0.0:
        return abs(spot - strike)
    root = vol * math.sqrt(years)
    d1 = (math.log(spot / strike) + 0.5 * vol * vol * years) / root
    d2 = d1 - root
    # C + P with a zero rate: S(2N(d1) - 1) - K(2N(d2) - 1). The intrinsic term is already inside the bracket
    # pair, so adding `strike - spot` on top of it -- as the first version did -- makes every out-of-the-money
    # straddle too expensive and inverts to a volatility that is too high.
    return spot * (2.0 * norm(d1) - 1.0) - strike * (2.0 * norm(d2) - 1.0)


def implied_vol(spot, strike, years, price):
    """Bisection on the straddle price, which is monotone in volatility and needs no greeks."""
    low, high = 0.01, 5.0
    for _ in range(80):
        middle = 0.5 * (low + high)
        if straddle_price(spot, strike, years, middle) > price:
            high = middle
        else:
            low = middle
    return 0.5 * (low + high)


def fetch_gz(url):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as response:
            return gzip.decompress(response.read()).decode()
    except (urllib.error.HTTPError, urllib.error.URLError):
        return None


def noon_of(day):
    return int(datetime(day.year, day.month, day.day, NOON, tzinfo=timezone.utc).timestamp()) * 1_000_000


def index_price(day):
    """The venue's own index, which is what an option settles against."""
    text = fetch_gz(f"{TARDIS}/derivative_ticker/{day.year}/{day.month:02d}/{day.day:02d}/BTC-PERPETUAL.csv.gz")
    if text is None:
        return None
    target = noon_of(day)
    for line in text.splitlines()[1:]:
        fields = line.split(",")
        if len(fields) < 11 or not fields[2].isdigit():
            continue
        if int(fields[2]) >= target and fields[9]:
            return float(fields[9])
    return None


def option_mid(day, symbol):
    text = fetch_gz(f"{TARDIS}/book_ticker/{day.year}/{day.month:02d}/{day.day:02d}/{symbol}.csv.gz")
    if text is None:
        return None
    target = noon_of(day)
    for line in text.splitlines()[1:]:
        fields = line.split(",")
        if len(fields) < 8 or not fields[2].isdigit() or int(fields[2]) < target:
            continue
        ask, bid = fields[5], fields[6]
        if not ask or not bid:
            continue
        ask, bid = float(ask), float(bid)
        if ask > 0 and bid > 0:
            return 0.5 * (ask + bid)
    return None


def fridays(day, horizon):
    out = []
    cursor = day + timedelta(days=1)
    while (cursor - day).days <= horizon:
        if cursor.weekday() == 4:
            out.append(cursor)
        cursor += timedelta(days=1)
    return out


def month_step(day):
    return date(day.year + 1, 1, 1) if day.month == 12 else date(day.year, day.month + 1, 1)


def daily_closes(year, month):
    url = f"{BINANCE}/BTCUSDT-1d-{year}-{month:02d}.zip"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=180) as response:
            blob = response.read()
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            text = archive.read(archive.namelist()[0]).decode()
    except (urllib.error.HTTPError, urllib.error.URLError, zipfile.BadZipFile):
        return {}
    closes = {}
    for line in text.splitlines():
        fields = line.split(",")
        if not fields[0][:1].isdigit():
            continue
        stamp = int(fields[0])
        seconds = stamp // 1000 if len(fields[0]) == 13 else (
            stamp // 1_000_000 if len(fields[0]) > 13 else stamp)
        closes[datetime.fromtimestamp(seconds, tz=timezone.utc).date()] = float(fields[4])
    return closes


def realised_volatility(start, end):
    """Annualised volatility of daily log returns over the window, or None when the window is not covered."""
    history = {}
    cursor = date(start.year, start.month, 1)
    while cursor <= end:
        history.update(daily_closes(cursor.year, cursor.month))
        cursor = month_step(cursor)
    prices = [history[key] for key in sorted(history) if start <= key <= end]
    if len(prices) < 10:
        return None, None
    returns = [math.log(prices[index + 1] / prices[index]) for index in range(len(prices) - 1)]
    volatility = math.sqrt(sum(value * value for value in returns) / len(returns) * 365)
    return volatility, prices[-1]


def collect(day):
    """One at-the-money straddle at noon: strike, premium in quote currency, implied volatility, and its expiry."""
    spot = index_price(day)
    if spot is None:
        return None
    for expiry in fridays(day, 45):
        if (expiry - day).days < 20:
            continue
        years = (expiry - day).days / 365.0
        suffix = f"{expiry.day:02d}{MONTHS[expiry.month]}{expiry.year % 100:02d}"
        for grid in (100, 250, 500):
            for offset in (0, -1, 1, -2, 2):
                strike = int(round(spot / grid) * grid) + offset * grid
                base = f"BTC-{suffix}-{strike}"
                call = option_mid(day, base + "-C")
                if call is None:
                    continue
                put = option_mid(day, base + "-P")
                if put is None:
                    continue
                premium_quote = (call + put) * spot
                return {"day": day, "expiry": expiry, "strike": strike, "spot": spot, "years": years,
                        "premium_quote": premium_quote,
                        "iv": implied_vol(spot, strike, years, premium_quote),
                        "moneyness": abs(strike - spot) / spot}
    return None


def settle(row):
    """The short straddle's result if it is held to expiry, in quote currency at the entry spot."""
    volatility, terminal = realised_volatility(row["day"], row["expiry"])
    if volatility is None:
        return None
    payoff = abs(terminal - row["strike"]) / terminal * row["spot"]
    return {"rv": volatility, "terminal": terminal, "payoff": payoff,
            "pnl": row["premium_quote"] - payoff}


def self_test():
    """The inversion must round-trip at and away from the money, and the at-the-money price must match its identity."""
    spot, years = 30_000.0, 30 / 365.0
    for vol in (0.30, 0.60, 1.10):
        for strike in (spot, spot * 0.97, spot * 1.04):
            price = straddle_price(spot, strike, years, vol)
            assert abs(implied_vol(spot, strike, years, price) - vol) < 1e-6, (vol, strike)
    # The at-the-money straddle is worth about 0.8 S sigma sqrt(T). That identity is what the first version of the
    # options section used *backwards* -- reading a volatility off a price struck up to six percent away -- and it is
    # checked here in the direction it is valid.
    atm = straddle_price(spot, spot, years, 0.6)
    assert abs(atm / (0.8 * spot * 0.6 * math.sqrt(years)) - 1.0) < 0.02, atm
    # A straddle struck away from the money is not cheaper: it carries intrinsic value, and the first self-test
    # asserted the opposite and failed for the right reason.
    assert straddle_price(spot, spot * 1.10, years, 0.6) > atm
    print("self-test OK: the inversion round-trips at and away from the money, and the at-the-money price "
          "matches 0.8 S sigma sqrt T")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start", default="2020-01", help="first month, as YYYY-MM")
    parser.add_argument("--end", default="", help="last month, as YYYY-MM; defaults to the start month")
    parser.add_argument("--output", default="", help="CSV to write")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0

    first = date(*map(int, args.start.split("-")), 1)
    last = date(*map(int, (args.end or args.start).split("-")), 1)
    rows = []
    day = first
    while day <= last:
        row = collect(day)
        if row is None:
            print(f"{day}: no at-the-money straddle found", file=sys.stderr)
        else:
            result = settle(row)
            if result is None:
                print(f"{day}: no closing prices for the window", file=sys.stderr)
            else:
                row.update(result)
                rows.append(row)
                print(f"{day} exp {row['expiry']} K {row['strike']:,} "
                      f"({(row['strike'] / row['spot'] - 1) * 100:+.1f} %) IV {row['iv'] * 100:.1f} % "
                      f"RV {row['rv'] * 100:.1f} % premium {row['premium_quote']:,.0f} "
                      f"payoff {row['payoff']:,.0f} pnl {row['pnl']:+,.0f}", flush=True)
        day = month_step(day)

    if args.output and rows:
        with open(args.output, "w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["day", "expiry", "strike", "spot", "iv", "rv", "premium", "payoff", "pnl",
                             "moneyness"])
            for row in rows:
                writer.writerow([row["day"], row["expiry"], row["strike"], row["spot"], f"{row['iv']:.6f}",
                                 f"{row['rv']:.6f}", f"{row['premium_quote']:.2f}", f"{row['payoff']:.2f}",
                                 f"{row['pnl']:.2f}", f"{row['moneyness']:.6f}"])
        print(f"wrote {args.output} with {len(rows)} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
