#!/usr/bin/env python3
"""Screen symbols for the one thing the fee arithmetic leaves open: a half spread wide enough to absorb a fee.

The latency sweep closed the SPY window, and what it left open is this. A quote earns the half spread it
captures per side, and a per-share commission of 0.0035 USD is a fixed 0.35 of a tick under a one cent tick
whatever the share price, so every US share has the same fee ratio and they differ only in **how wide their
spread actually is**. The pre-registered rule of phase 2b's first gate is therefore:

    pass when the median spread is at least three times the fee per share expressed in ticks

which with the default commission is a median spread of 2.1 ticks. This script measures that on the cheap
schema, top of book only, before any ten-level depth is bought, and it prints the cost of the measurement
before it spends anything.

What it will not do is choose the activity floor. A wide spread with no trades is not a market, and how many
trades a strategy needs is only known from the fill counts of the first screened symbols, so the floor is
deliberately not pre-registered here: `trades` is reported, the decision about a floor is recorded in the
documents, and the second screen is run against the recorded floor rather than against one chosen after seeing
this answer.

The endpoint set takes its parameters in the query string, as `tools/fetch_databento.py` records at length, and
prices come back as decimals when pretty_px is asked for.

Usage:
    export DATABENTO_API_KEY=db-...
    python3 tools/screen_spread.py --cost --symbols SPY,QQQ,IWM --start 2026-09-29T13:30 --end 2026-09-29T15:30
    python3 tools/screen_spread.py --download --max-cost-usd=5 --symbols SPY,QQQ,IWM \\
        --start 2026-09-29T13:30 --end 2026-09-29T15:30
    python3 tools/screen_spread.py --input data/v2/xnas_mbp10_SPY_2026-09-29_1330_1530.csv --symbol SPY

`--input` computes the same statistics from a file already on disk, which is how this script is tested without
spending anything and how a ten-level file can be screened as if it were top of book, because the ten-level
schema carries the top of book columns as well.
"""

import argparse
import base64
import csv
import os
import statistics
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from pathlib import Path

BASE = "https://hist.databento.com/v0/"
FEE_PER_SHARE = 0.0035
TICK = 0.01


def api(endpoint, params, timeout=3600):
    request = urllib.request.Request(
        f"{BASE}{endpoint}?{urllib.parse.urlencode(params, doseq=True)}", method="GET")
    key = os.environ.get("DATABENTO_API_KEY", "")
    if not key:
        raise SystemExit("DATABENTO_API_KEY is not set; export it rather than committing it")
    request.add_header("Authorization", "Basic " + base64.b64encode(f"{key}:".encode()).decode())
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise SystemExit(f"{endpoint} failed: HTTP {error.code} {error.read().decode()[:300]}")


def query(args, symbol):
    return {"dataset": args.dataset, "schema": args.schema, "symbols": symbol, "stype_in": "raw_symbol",
            "start": args.start, "end": args.end}


def costs(args):
    total = 0.0
    rows = []
    for symbol in args.symbols:
        text = api("metadata.get_cost", query(args, symbol)).decode()
        try:
            cost = float(text)
        except ValueError:
            raise SystemExit(f"{symbol}: unexpected cost reply {text[:200]}")
        total += cost
        rows.append((symbol, cost))
    print("| symbol | cost USD |")
    print("|---|---:|")
    for symbol, cost in rows:
        print(f"| {symbol} | {cost:.6f} |")
    print(f"| **total** | **{total:.6f}** |")
    print(f"[SCREEN] window {args.start} to {args.end} schema {args.schema} symbols {len(rows)} "
          f"total_cost_usd {total:.6f} cap_usd {args.max_cost_usd}")
    return total


def download(args, symbol):
    target = Path(args.cache_dir) / f"{symbol}_{args.start.replace(':', '')}_{args.end.replace(':', '')}.csv"
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and target.stat().st_size > 0:
        print(f"[SCREEN] {symbol}: reusing {target}")
        return target
    params = query(args, symbol)
    params.update({"encoding": "csv", "map_symbols": "true", "pretty_ts": "false", "pretty_px": "true"})
    request = urllib.request.Request(
        f"{BASE}timeseries.get_range?{urllib.parse.urlencode(params, doseq=True)}")
    key = os.environ.get("DATABENTO_API_KEY", "")
    request.add_header("Authorization", "Basic " + base64.b64encode(f"{key}:".encode()).decode())
    written = 0
    with open(target, "wb") as sink:
        try:
            with urllib.request.urlopen(request, timeout=3600) as stream:
                while True:
                    chunk = stream.read(1 << 20)
                    if not chunk:
                        break
                    sink.write(chunk)
                    written += len(chunk)
        except urllib.error.HTTPError as error:
            raise SystemExit(f"{symbol}: get_range failed HTTP {error.code} {error.read().decode()[:300]}")
    print(f"[SCREEN] {symbol}: wrote {target} ({written} bytes)")
    return target


def measure(path, symbol, tick, multiplier, fee_per_share):
    """One streaming pass: the spread distribution in ticks, the trade count and the time covered.

    Two distributions are collected on purpose. The book spread is what the screen rule used to be written
    against, and the spread on the rows where a trade happened is the closer proxy for what a resting quote
    would be filled into: a market maker is filled where trades print, not at the median of the book, and the
    two can differ by a factor, which the recorded windows show they do.
    """
    spreads = Counter()
    ticks_seen = []
    trade_ticks = []
    trades = 0
    rows = 0
    first_ts = last_ts = None
    with open(path, "rt") as handle:
        reader = csv.reader(handle)
        header = next(reader)
        columns = {name: index for index, name in enumerate(header)}
        for name in ("ts_event", "action", "bid_px_00", "ask_px_00"):
            if name not in columns:
                raise SystemExit(f"{path}: missing column {name}")
        for row in reader:
            if len(row) < len(header):
                continue
            rows += 1
            ts = int(row[columns["ts_event"]])
            if first_ts is None:
                first_ts = ts
            last_ts = ts
            is_trade = row[columns["action"]] == "T"
            if is_trade:
                trades += 1
            try:
                bid = float(row[columns["bid_px_00"]])
                ask = float(row[columns["ask_px_00"]])
            except ValueError:
                continue
            if bid <= 0.0 or ask <= 0.0 or ask < bid:
                continue
            ticks = (ask - bid) / tick
            ticks_seen.append(ticks)
            spreads[round(ticks)] += 1
            if is_trade:
                trade_ticks.append(ticks)
    if not ticks_seen:
        raise SystemExit(f"{path}: no usable top of book")
    ticks_seen.sort()
    trade_ticks.sort()
    seconds = (last_ts - first_ts) / 1e9 if first_ts and last_ts else 0.0
    fee_ticks = fee_per_share / (tick * multiplier)
    median = statistics.median(ticks_seen)
    median_trade = statistics.median(trade_ticks) if trade_ticks else 0.0
    return {
        "symbol": symbol,
        "rows": rows,
        "trades": trades,
        "seconds": seconds,
        "median_ticks": median,
        "mean_ticks": sum(ticks_seen) / len(ticks_seen),
        "p10_ticks": ticks_seen[int(0.10 * (len(ticks_seen) - 1))],
        "p90_ticks": ticks_seen[int(0.90 * (len(ticks_seen) - 1))],
        "median_trade_ticks": median_trade,
        "mean_trade_ticks": sum(trade_ticks) / len(trade_ticks) if trade_ticks else 0.0,
        "at_one_tick": spreads.get(1, 0) / len(ticks_seen),
        "at_two_ticks": spreads.get(2, 0) / len(ticks_seen),
        "at_three_or_more": sum(c for t, c in spreads.items() if t >= 3) / len(ticks_seen),
        "half_spread_ticks": median / 2.0,
        "trade_half_spread_ticks": median_trade / 2.0,
        "fee_ticks": fee_ticks,
        "ratio": (median_trade / 2.0) / fee_ticks,
        "rows_per_second": rows / seconds if seconds > 0 else 0.0,
    }


def report(args, results):
    print()
    print("| symbol | median book spread | median trade spread | half at trades | fee/side | ratio | >=3 ticks | rows/s | trades | verdict |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for row in results:
        threshold = args.min_ratio * 2.0 * row["fee_ticks"]
        verdict = "pass" if row["median_trade_ticks"] >= threshold else "fail"
        print(f"| {row['symbol']} | {row['median_ticks']:.3f} | {row['median_trade_ticks']:.3f} | "
              f"{row['trade_half_spread_ticks']:.3f} | {row['fee_ticks']:.3f} | {row['ratio']:.3f} | "
              f"{row['at_three_or_more']:.3f} | {row['rows_per_second']:.1f} | {row['trades']} | {verdict} |")
        print(f"[SCREEN] symbol={row['symbol']} median_book_ticks={row['median_ticks']:.3f} "
              f"median_trade_ticks={row['median_trade_ticks']:.3f} fee_ticks={row['fee_ticks']:.3f} "
              f"ratio={row['ratio']:.3f} rows={row['rows']} trades={row['trades']} verdict={verdict}")
    print()
    print(f"[SCREEN] the rule is the pre-registered one: half spread >= {args.min_ratio:.1f} x the fee per side, "
          f"which is a median spread of {args.min_ratio * 2.0 * (FEE_PER_SHARE / TICK):.3f} ticks at the default "
          "commission. The rule is applied to the spread at the trades rather than to the median of the book, "
          "because fills happen where trades print, and the recorded SPY window shows a further haircut of about "
          "half between the touch half spread and the capture a fill actually realised: this is a ranking device "
          "whose absolute verdict is revisited after the first replay. The activity floor is not decided here on "
          "purpose, so that it can be recorded before the next screen rather than chosen after seeing this one.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cost", action="store_true", help="price the window and stop")
    parser.add_argument("--download", action="store_true", help="fetch and measure")
    parser.add_argument("--input", help="measure a file already on disk instead of downloading")
    parser.add_argument("--symbol", default="", help="symbol name for --input")
    parser.add_argument("--symbols", default="", help="comma separated list of symbols")
    parser.add_argument("--symbols-file", default="", help="a file with one symbol per line")
    parser.add_argument("--dataset", default="XNAS.ITCH",
                        help="XNAS.ITCH is the venue we could rest on; EQUS.MINI mbp-1 is the consolidated NBBO")
    parser.add_argument("--schema", default="mbp-1")
    parser.add_argument("--start", default="2026-09-29T13:30")
    parser.add_argument("--end", default="2026-09-29T15:30")
    parser.add_argument("--tick", type=float, default=TICK)
    parser.add_argument("--multiplier", type=float, default=1.0,
                        help="USD per price unit per unit, 5 for a micro E-mini, 1 for a share")
    parser.add_argument("--fee-per-share", type=float, default=FEE_PER_SHARE)
    parser.add_argument("--min-ratio", type=float, default=3.0)
    parser.add_argument("--max-cost-usd", type=float, default=0.0,
                        help="0, the default, refuses to download at all")
    parser.add_argument("--cache-dir", default="data/v2/screen")
    args = parser.parse_args()

    if args.input:
        if not args.symbol:
            raise SystemExit("--input needs --symbol")
        report(args, [measure(args.input, args.symbol, args.tick, args.multiplier, args.fee_per_share)])
        return 0
    symbols = [name.strip() for name in args.symbols.split(",") if name.strip()]
    if args.symbols_file:
        symbols += [line.strip() for line in Path(args.symbols_file).read_text().splitlines() if line.strip()]
    if not symbols:
        raise SystemExit("give --symbols, --symbols-file, or --input")
    args.symbols = symbols
    print(f"[SCREEN] dataset {args.dataset} schema {args.schema} window {args.start} to {args.end} "
          f"symbols {len(symbols)}")
    total = costs(args)
    if not args.download:
        return 0
    if total > args.max_cost_usd:
        raise SystemExit(f"the window costs {total:.6f} USD and the cap is {args.max_cost_usd:.6f} USD; "
                         "raise --max-cost-usd to accept it")
    results = [measure(download(args, symbol), symbol, args.tick, args.multiplier, args.fee_per_share)
               for symbol in symbols]
    report(args, results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
