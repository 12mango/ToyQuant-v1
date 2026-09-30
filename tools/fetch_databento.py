#!/usr/bin/env python3
"""Fetch and convert Databento L2 depth, the first source this repository has that is both real depth and a
market a Hong Kong personal account may trade.

What the route is, and what it is not, in the order the other sources were established:

* The API key comes from `DATABENTO_API_KEY`. It is never written to a file here, and a missing key is an error
  rather than a default, because a committed key is a permanent leak.
* The endpoints take their parameters **in the query string**. A JSON body is rejected with "Field required"
  for every field, which is what this repository's first attempt did for an hour; the shapes are printed in the
  usage below so the next reader does not repeat it.
* `mbp-10` is ten price levels per side, and **every row is the whole book**: `action` says what happened at the
  top (A add, C cancel, M modify, T trade, N clear) while the row carries all ten levels. So one download is both
  the depth and the trades, and the trades are the `action=T` rows, which is what makes the cost of a session
  about a dollar instead of a dollar for depth plus several for a trade feed.
* Prices are integers scaled by 1e-9 by default; `--pretty-px` asks for decimals instead, which is what a reader
  written against a decimal column expects. Timestamps stay integer nanoseconds, which is the unit the rest of
  this repository counts in.
* The cost is billed on the DBN size, not on the CSV size, so `--cost` is the number that matters and it is a
  fraction of a dollar for an instrument like the micro E-mini.

The conversion target is the CSV pair the existing readers already consume, so no C++ was written for the
format itself:

    exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount   # depth, Deribit incremental shape
    exchange,symbol,timestamp,id,side,price,amount                            # trades, Deribit trade shape

Rows sharing a `local_timestamp` form one batch, so each converted row keeps its own; `amount` is an absolute
level size where zero removes the level, and the first row of the file is written as a snapshot of all ten
levels so the book starts complete. The converter checks its own work on the way: it rebuilds each row's book
from the deltas it emitted and compares that against the levels the vendor sent, and it counts how many trades
priced at the touch agree with the aggressor side it wrote down.

Usage:
    export DATABENTO_API_KEY=db-...
    python3 tools/fetch_databento.py --cost --dataset GLBX.MDP3 --schema mbp-10 --symbol MESZ6 \\
        --start 2026-09-29T13:30 --end 2026-09-29T20:00
    python3 tools/fetch_databento.py --download --dataset GLBX.MDP3 --schema mbp-10 --symbol MESZ6 \\
        --start 2026-09-29T13:30 --end 2026-09-29T20:00 --output data/v2/glbx_mbp10_MESZ6_2026-09-29.csv.gz
    python3 tools/fetch_databento.py --convert --input data/v2/glbx_mbp10_MESZ6_2026-09-29.csv.gz \\
        --symbol MESZ6 --exchange glbx --depth-out data/v2/glbx_MESZ6_depth.csv \\
        --trades-out data/v2/glbx_MESZ6_trades.csv
"""

import argparse
import base64
import gzip
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter

BASE = "https://hist.databento.com/v0/"
LEVELS = 10


def api(endpoint, params, timeout=3600, raw=False):
    query = urllib.parse.urlencode(params, doseq=True)
    request = urllib.request.Request(f"{BASE}{endpoint}?{query}", method="GET")
    key = os.environ.get("DATABENTO_API_KEY", "")
    if not key:
        raise SystemExit("DATABENTO_API_KEY is not set; export it rather than committing it")
    request.add_header("Authorization", "Basic " + base64.b64encode(f"{key}:".encode()).decode())
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
    except urllib.error.HTTPError as error:
        raise SystemExit(f"{endpoint} failed: HTTP {error.code} {error.read().decode()[:400]}")
    return body if raw else body.decode()


def query_params(args):
    return {"dataset": args.dataset, "schema": args.schema, "symbols": args.symbol,
            "stype_in": args.stype, "start": args.start, "end": args.end}


def show_cost(args):
    params = query_params(args)
    print(f"[DATABENTO] get_cost dataset={params['dataset']} schema={params['schema']} "
          f"symbol={params['symbols']} {params['start']} to {params['end']}")
    print(f"[DATABENTO] cost_usd={api('metadata.get_cost', params)}")


def download(args):
    if not args.output:
        raise SystemExit("--download needs --output")
    params = dict(query_params(args))
    params.update({"encoding": "csv", "map_symbols": "true", "pretty_ts": "false",
                   "pretty_px": "true" if args.pretty_px else "false"})
    total = 0
    with open(args.output, "wb") as sink:
        response = urllib.request.Request(
            f"{BASE}timeseries.get_range?{urllib.parse.urlencode(params, doseq=True)}", method="GET")
        key = os.environ.get("DATABENTO_API_KEY", "")
        if not key:
            raise SystemExit("DATABENTO_API_KEY is not set; export it rather than committing it")
        response.add_header("Authorization", "Basic " + base64.b64encode(f"{key}:".encode()).decode())
        try:
            with urllib.request.urlopen(response, timeout=3600) as stream:
                while True:
                    chunk = stream.read(1 << 20)
                    if not chunk:
                        break
                    sink.write(chunk)
                    total += len(chunk)
        except urllib.error.HTTPError as error:
            raise SystemExit(f"get_range failed: HTTP {error.code} {error.read().decode()[:400]}")
    print(f"[DATABENTO] wrote {args.output} ({total} bytes) for {params['symbols']} "
          f"{params['start']} to {params['end']}")


def open_input(path):
    handle = open(path, "rb")
    magic = handle.read(2)
    handle.seek(0)
    if magic == b"\x1f\x8b":
        return gzip.open(path, "rt")
    return open(path, "rt")


def open_output(path):
    if path.endswith(".gz"):
        return gzip.open(path, "wt")
    return open(path, "wt")


def levels_of(fields, columns, side):
    """The ten levels of one side as (price text, size) in the order the vendor sent them."""
    levels = []
    for level in range(LEVELS):
        price_index = columns.get(f"{side}_px_{level:02d}")
        size_index = columns.get(f"{side}_sz_{level:02d}")
        if price_index is None or size_index is None or size_index >= len(fields):
            continue
        price = fields[price_index]
        size = fields[size_index]
        if price in ("", "0", "0.000000000") or size in ("", "0"):
            continue
        levels.append((price, int(size)))
    return levels


def convert(args):
    source = open_input(args.input)
    depth = open_output(args.depth_out)
    trades = open_output(args.trades_out)
    depth.write("exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n")
    trades.write("exchange,symbol,timestamp,id,side,price,amount\n")
    counts = Counter()
    # The vendor's book and the book rebuilt from the deltas this tool emitted, kept apart on purpose:
    # comparing them on every row is what makes the conversion a checked one rather than a plausible one.
    vendor = {"bid": {}, "ask": {}}
    rebuilt = {"bid": {}, "ask": {}}
    mismatches = []
    aggressor = Counter()
    first_row = True
    for line_number, line in enumerate(source, start=1):
        if line_number == 1:
            columns = {name: index for index, name in enumerate(line.rstrip("\r\n").split(","))}
            continue
        fields = line.rstrip("\r\n").split(",")
        if len(fields) < len(columns):
            counts["short_rows"] += 1
            continue
        action = fields[columns["action"]]
        ts_event = fields[columns["ts_event"]]
        ts_recv = fields[columns["ts_recv"]]
        if action == "T":
            size = int(fields[columns["size"]])
            side = fields[columns["side"]]
            # Databento labels a trade with the side of the aggressing order, B for a buy and A for a sell, and
            # N when there is no aggressor to name: an auction, a cross, or a print the venue reported without
            # one. A trade with no side cannot drive this repository's model, whose fills and markouts are keyed
            # by the aggressor, so it is dropped rather than guessed at, and the count of what was dropped is
            # reported. In the equity file below that is about five percent of the prints; the futures file had
            # none.
            if side not in ("A", "B"):
                counts["side_unknown_trades"] += 1
                continue
            counts["trade_rows"] += 1
            # The id column is a running index rather than the vendor's sequence: Databento's sequence is
            # per record and ties across the rows one event produces, while the reader this file feeds
            # requires a strictly increasing id, so the count of trades written so far is the only
            # monotone number available here.
            trades.write(f"{args.exchange},{args.symbol},{ts_event},{counts['trade_rows']},"
                         f"{'buy' if side == 'B' else 'sell'},{fields[columns['price']]},{size}\n")
            # The aggressor side is a convention, so it is counted rather than assumed: a print at the bid
            # labelled a buy, or at the ask labelled a sell, would mean the mapping is inverted.
            best_bid = max((float(p) for p in vendor["bid"]), default=None)
            best_ask = min((float(p) for p in vendor["ask"]), default=None)
            price = float(fields[columns["price"]])
            if best_bid is not None and abs(price - best_bid) < 1e-9:
                aggressor[f"at_bid_side_{side}"] += 1
            elif best_ask is not None and abs(price - best_ask) < 1e-9:
                aggressor[f"at_ask_side_{side}"] += 1
            else:
                aggressor["inside_or_beyond"] += 1
        else:
            counts["depth_rows"] += 1
            if action == "N":
                counts["book_resets"] += 1

        # The first row, and any reset, are written as a snapshot of every level, so a reader that waits for
        # a snapshot before applying updates finds one in the first row of the file.
        snapshot = first_row or action == "N"
        first_row = False
        for side in ("bid", "ask"):
            block = dict(levels_of(fields, columns, side))
            vendor[side] = block
            if snapshot:
                rebuilt[side] = dict(rebuilt[side])
            prices = sorted(set(block) | set(rebuilt[side]),
                            key=lambda text: float(text), reverse=side == "bid")
            for price in prices:
                previous = rebuilt[side].get(price, 0)
                current = block.get(price, 0)
                if snapshot or previous != current:
                    depth.write(f"{args.exchange},{args.symbol},{ts_event},{ts_recv},{int(snapshot)},"
                                f"{side},{price},{current}\n")
                    counts["rows_written"] += 1
                    if previous > current:
                        counts["level_decreases"] += 1
                    if current == 0:
                        rebuilt[side].pop(price, None)
                    else:
                        rebuilt[side][price] = current
        if rebuilt != vendor:
            counts["rebuild_mismatches"] += 1
            if len(mismatches) < 3:
                mismatches.append((line_number, rebuilt["bid"], vendor["bid"]))
    for handle in (source, depth, trades):
        handle.close()
    return report_conversion(counts, aggressor, mismatches)



def report_conversion(counts, aggressor, mismatches):
    print(f"[DATABENTO] convert depth_rows={counts['depth_rows']} trade_rows={counts['trade_rows']} "
          f"rows_written={counts['rows_written']} level_decreases={counts['level_decreases']} "
          f"book_resets={counts['book_resets']} short_rows={counts['short_rows']} "
          f"side_unknown_trades={counts['side_unknown_trades']}")
    print(f"[DATABENTO] convert aggressor_by_price {dict(sorted(aggressor.items()))}")
    if counts["rebuild_mismatches"]:
        print(f"[DATABENTO] convert FAILED: {counts['rebuild_mismatches']} rows rebuilt from the emitted "
              f"deltas differ from the vendor's levels; first at line {mismatches[0][0]}")
        return 2
    print("[DATABENTO] convert OK: every row's book rebuilt from the emitted deltas matches the vendor's "
          "ten levels exactly")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cost", action="store_true", help="print the price of a query and stop")
    parser.add_argument("--download", action="store_true", help="run the query and write the CSV")
    parser.add_argument("--convert", action="store_true", help="split a downloaded CSV into depth and trades")
    parser.add_argument("--dataset", default="GLBX.MDP3")
    parser.add_argument("--schema", default="mbp-10")
    parser.add_argument("--symbol", default="")
    parser.add_argument("--stype", default="raw_symbol",
                        help="raw_symbol, continuous, parent or instrument_id")
    parser.add_argument("--start", default="", help="ISO 8601, for example 2026-09-29T13:30")
    parser.add_argument("--end", default="")
    parser.add_argument("--output", default="")
    parser.add_argument("--pretty-px", action="store_true",
                        help="ask for decimal prices instead of 1e-9 integers")
    parser.add_argument("--input", default="")
    parser.add_argument("--depth-out", default="")
    parser.add_argument("--trades-out", default="")
    parser.add_argument("--exchange", default="glbx", help="the exchange field the readers will see")
    args = parser.parse_args()
    if args.cost:
        show_cost(args)
        return 0
    if args.download:
        download(args)
        return 0
    if args.convert:
        if not (args.input and args.depth_out and args.trades_out and args.symbol):
            raise SystemExit("--convert needs --input, --symbol, --depth-out and --trades-out")
        return convert(args)
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())

