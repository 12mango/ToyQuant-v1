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
trades a strategy needs is only known from the fill counts of the first screened symbols, so the floor is not
chosen here: it is recorded in `docs/ROADMAP.md`, and this script applies it when `--min-trades-per-second`
is given, printing `trades_per_second` and a verdict of its own next to the spread verdict. A screen run with
that flag is the second screen the floor paragraph promises, measured against a number that was written down
before the answer was in.

The endpoint set takes its parameters in the query string, as `tools/fetch_databento.py` records at length, and
prices come back as decimals when pretty_px is asked for.

Usage:
    export DATABENTO_API_KEY=db-...
    python3 tools/screen_spread.py --cost --symbols SPY,QQQ,IWM --start 2026-09-29T13:30 --end 2026-09-29T15:30
    python3 tools/screen_spread.py --download --max-cost-usd=5 --symbols SPY,QQQ,IWM \\
        --start 2026-09-29T13:30 --end 2026-09-29T15:30
    python3 tools/screen_spread.py --input data/v2/xnas_mbp10_SPY_2026-09-29_1330_1530.csv --symbol SPY
    python3 tools/screen_spread.py --input data/v2/xnas_mbp10_SPY_2026-09-29_1330_1530.csv --symbol SPY \\
        --fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013

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
# The account's schedule, all zero until named, so a run without the flag reproduces the recorded table exactly:
# the keys match the engine's own --fee-schedule, so one string prices the screen and the replay the same way.
SCHEDULE_DEFAULTS = {"min": 0.0, "regulatory": 0.0, "clearing": 0.0, "audit": 0.0, "rebate": 0.0}


def parse_schedule(text):
    schedule = dict(SCHEDULE_DEFAULTS)
    for item in [piece for piece in text.split(",") if piece.strip()]:
        if "=" not in item:
            raise SystemExit(f"--fee-schedule: {item!r} is not KEY=VALUE")
        key, value = item.split("=", 1)
        if key.strip() not in schedule:
            raise SystemExit(f"--fee-schedule: {key.strip()!r} is not one of "
                             f"{', '.join(sorted(schedule))}")
        try:
            schedule[key.strip()] = float(value)
        except ValueError:
            raise SystemExit(f"--fee-schedule: {value!r} is not a number")
    return schedule


def schedule_per_side(schedule, price, fee_per_share, tick, multiplier):
    """The account's charge for one unit filled on one side, in ticks, at a given share price.

    The commission, the clearing fee and the audit fee are charged a share whichever side it is; the rebate is
    credited on the fills that add liquidity, which is the side a market maker is on; and the regulatory fee is
    charged on the proceeds of a sale, so it enters halved here, because a market maker sells on half its fills.
    That halving is what puts the share price into a rule that used to be the same for every symbol.
    """
    unit = fee_per_share + schedule["clearing"] + schedule["audit"] - schedule["rebate"]
    unit += 0.5 * schedule["regulatory"] * price
    return unit / (tick * multiplier)


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


def measure(path, symbol, tick, multiplier, fee_per_share, schedule=None):
    """One streaming pass: the spread distribution in ticks, the trade count and the time covered.

    Two distributions are collected on purpose. The book spread is what the screen rule used to be written
    against, and the spread on the rows where a trade happened is the closer proxy for what a resting quote
    would be filled into: a market maker is filled where trades print, not at the median of the book, and the
    two can differ by a factor, which the recorded windows show they do.
    """
    spreads = Counter()
    ticks_seen = []
    trade_ticks = []
    trade_prices = []
    trades = 0
    rows = 0
    first_ts = last_ts = None
    with open(path, "rt") as handle:
        reader = csv.reader(handle)
        header = next(reader)
        columns = {name: index for index, name in enumerate(header)}
        for name in ("ts_event", "action", "bid_px_00", "ask_px_00", "price"):
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
                try:
                    trade_prices.append(float(row[columns["price"]]))
                except ValueError:
                    pass
    if not ticks_seen:
        raise SystemExit(f"{path}: no usable top of book")
    ticks_seen.sort()
    trade_ticks.sort()
    seconds = (last_ts - first_ts) / 1e9 if first_ts and last_ts else 0.0
    fee_ticks = fee_per_share / (tick * multiplier)
    # The share price is what the second rule needs: the regulatory fee is charged on the proceeds of a sale, so
    # the same spread in ticks is a different round turn on a ten dollar share and on a three hundred dollar one.
    median_trade_price = statistics.median(trade_prices) if trade_prices else 0.0
    schedule_ticks = (schedule_per_side(schedule, median_trade_price, fee_per_share, tick, multiplier)
                      if schedule else fee_ticks)
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
        "price": median_trade_price,
        "schedule_ticks": schedule_ticks,
        "round_turn_ticks": 2.0 * schedule_ticks,
        "schedule_ratio": (median_trade / 2.0) / schedule_ticks if schedule_ticks > 0 else 0.0,
        "ratio": (median_trade / 2.0) / fee_ticks,
        "rows_per_second": rows / seconds if seconds > 0 else 0.0,
        "trades_per_second": trades / seconds if seconds > 0 else 0.0,
    }


def report(args, results):
    print()
    floor = args.min_trades_per_second
    print("| symbol | price USD | median book spread | median trade spread | half at trades | fee/side | "
          "schedule/side | round turn | ratio | schedule ratio | >=3 ticks | rows/s | trades | trades/s | "
          "spread | spread, schedule | activity |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|")
    for row in results:
        threshold = args.min_ratio * 2.0 * row["fee_ticks"]
        verdict = "pass" if row["median_trade_ticks"] >= threshold else "fail"
        schedule_threshold = args.min_ratio * row["round_turn_ticks"]
        schedule_verdict = "pass" if row["median_trade_ticks"] >= schedule_threshold else "fail"
        activity = "-" if floor <= 0 else ("pass" if row["trades_per_second"] >= floor else "fail")
        print(f"| {row['symbol']} | {row['price']:.2f} | {row['median_ticks']:.3f} | "
              f"{row['median_trade_ticks']:.3f} | {row['trade_half_spread_ticks']:.3f} | "
              f"{row['fee_ticks']:.3f} | {row['schedule_ticks']:.3f} | {row['round_turn_ticks']:.3f} | "
              f"{row['ratio']:.3f} | {row['schedule_ratio']:.3f} | {row['at_three_or_more']:.3f} | "
              f"{row['rows_per_second']:.1f} | {row['trades']} | {row['trades_per_second']:.2f} | "
              f"{verdict} | {schedule_verdict} | {activity} |")
        print(f"[SCREEN] symbol={row['symbol']} median_book_ticks={row['median_ticks']:.3f} "
              f"median_trade_ticks={row['median_trade_ticks']:.3f} fee_ticks={row['fee_ticks']:.3f} "
              f"ratio={row['ratio']:.3f} rows={row['rows']} trades={row['trades']} verdict={verdict} "
              f"trades_per_second={row['trades_per_second']:.3f} activity_floor={floor:.3f} "
              f"activity_verdict={activity}")
        print(f"[SCREEN] schedule symbol={row['symbol']} price={row['price']:.6f} "
              f"schedule_ticks={row['schedule_ticks']:.6f} round_turn_ticks={row['round_turn_ticks']:.6f} "
              f"schedule_ratio={row['schedule_ratio']:.6f} threshold_ticks={schedule_threshold:.6f} "
              f"verdict={schedule_verdict}")
    print()
    print(f"[SCREEN] the rule is the pre-registered one: half spread >= {args.min_ratio:.1f} x the fee per side, "
          f"which is a median spread of {args.min_ratio * 2.0 * (FEE_PER_SHARE / TICK):.3f} ticks at the default "
          "commission. The rule is applied to the spread at the trades rather than to the median of the book, "
          "because fills happen where trades print, and the recorded SPY window shows a further haircut of about "
          "half between the touch half spread and the capture a fill actually realised: this is a ranking device "
          "whose absolute verdict is revisited after the first replay. The activity floor is the second "
          "pre-registered number, recorded in docs/ROADMAP.md from the fill counts of the first two screened "
          "symbols; it is applied only when --min-trades-per-second is given, because a floor named in a command "
          "line is a number that was written down before the answer was in.")
    if args.fee_schedule:
        print(f"[SCREEN] the second rule is the pre-registered one too: the spread at the trades against "
              f"{args.min_ratio:.1f} times the round turn of the account's own schedule at each share's price, "
              f"schedule {args.fee_schedule}. The minimum per order is reported and not charged, because it is "
              f"charged per order instance and this tool sees no orders; it is measured in the engine on whatever "
              f"symbol passes here, and the rule and its two written-down consequences are in docs/ROADMAP.md.")


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
    parser.add_argument("--fee-schedule", default="",
                        help="the account's schedule as KEY=VALUE pairs, the same keys as the engine's flag: "
                             "min, regulatory, clearing, audit, rebate. Empty, the default, applies the "
                             "commission alone, which is what the recorded table was measured under. With a "
                             "schedule the second verdict compares the spread at the trades to the round turn of "
                             "the account's own terms at that share's price, and the minimum is reported but not "
                             "charged, because it is per order instance and top of book cannot see orders")
    parser.add_argument("--min-ratio", type=float, default=3.0)
    parser.add_argument("--min-trades-per-second", type=float, default=0.0,
                        help="the activity floor recorded in docs/ROADMAP.md; 0, the default, reports "
                             "trades_per_second without a verdict")
    parser.add_argument("--max-cost-usd", type=float, default=0.0,
                        help="0, the default, refuses to download at all")
    parser.add_argument("--cache-dir", default="data/v2/screen")
    args = parser.parse_args()

    if args.min_trades_per_second < 0.0:
        raise SystemExit("--min-trades-per-second cannot be negative")
    schedule = parse_schedule(args.fee_schedule) if args.fee_schedule else None

    if args.input:
        if not args.symbol:
            raise SystemExit("--input needs --symbol")
        report(args, [measure(args.input, args.symbol, args.tick, args.multiplier, args.fee_per_share,
                             schedule)])
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
    results = [measure(download(args, symbol), symbol, args.tick, args.multiplier, args.fee_per_share,
                       schedule)
               for symbol in symbols]
    report(args, results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
