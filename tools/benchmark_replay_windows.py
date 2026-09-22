#!/usr/bin/env python3
"""Run the replay benchmark on time windows without loading the full files."""

import argparse
import csv
import re
import subprocess
import tempfile
from pathlib import Path


def header_index(header, names):
    lowered = {name.strip().lower(): index for index, name in enumerate(header)}
    for name in names:
        if name in lowered:
            return lowered[name]
    raise ValueError(f"missing columns: {names}")


def bounds(path, timestamp_names):
    with path.open(newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader)
        timestamp_index = header_index(header, timestamp_names)
        first = last = None
        for row in reader:
            if not row:
                continue
            timestamp = int(row[timestamp_index])
            first = timestamp if first is None else min(first, timestamp)
            last = timestamp if last is None else max(last, timestamp)
    if first is None or last is None:
        raise ValueError(f"empty input: {path}")
    return first, last


def write_window(source, target, timestamp_names, start, end):
    with source.open(newline="") as input_stream, target.open("w", newline="") as output_stream:
        reader = csv.reader(input_stream)
        writer = csv.writer(output_stream)
        header = next(reader)
        timestamp_index = header_index(header, timestamp_names)
        writer.writerow(header)
        for row in reader:
            if not row:
                continue
            timestamp = int(row[timestamp_index])
            if start <= timestamp < end:
                writer.writerow(row)


def benchmark(binary, trades, quotes, symbol, scale):
    output = subprocess.run(
        [binary, str(trades), str(quotes), symbol, str(scale)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout
    rows = []
    for line in output.splitlines():
        if re.match(r"^(l1|active_l1)\s", line):
            rows.append(" ".join(line.split()))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trades", type=Path)
    parser.add_argument("quotes", type=Path)
    parser.add_argument("--binary", default="./out/build/linux-debug/strategy_benchmark")
    parser.add_argument("--symbol", default="BTCUSDT")
    parser.add_argument("--scale", default="1000000")
    parser.add_argument("--windows", type=int, default=6)
    args = parser.parse_args()

    trade_first, trade_last = bounds(args.trades, ("transact_time", "timestamp", "time"))
    quote_first, quote_last = bounds(args.quotes, ("transaction_time", "timestamp", "time"))
    start = min(trade_first, quote_first)
    end = max(trade_last, quote_last) + 1
    width = max(1, (end - start + args.windows - 1) // args.windows)

    with tempfile.TemporaryDirectory(prefix="toy_quant_windows_") as directory:
        root = Path(directory)
        for number in range(args.windows):
            window_start = start + number * width
            window_end = min(end, window_start + width)
            trade_file = root / f"trades_{number}.csv"
            quote_file = root / f"quotes_{number}.csv"
            write_window(args.trades, trade_file, ("transact_time", "timestamp", "time"),
                         window_start, window_end)
            write_window(args.quotes, quote_file, ("transaction_time", "timestamp", "time"),
                         window_start, window_end)
            if trade_file.stat().st_size <= 1 or quote_file.stat().st_size <= 1:
                continue
            print(f"window={number + 1} start={window_start} end={window_end}")
            for row in benchmark(args.binary, trade_file, quote_file, args.symbol, args.scale):
                print(row)


if __name__ == "__main__":
    main()
