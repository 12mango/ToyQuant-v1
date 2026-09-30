#!/usr/bin/env python3
"""Cut a CSV or CSV.gz down to the rows whose numeric column falls in a closed range.

The gate needs windows a tracked file does not contain: ten minutes around one funding settlement, so that a
replay is holding a position when the settlement arrives. `head` cannot express that, and a hand-written awk
in a shell script cannot be read back from the document that quotes the result, so this is the one-line form.

Usage:
    python3 tools/slice_csv_window.py --input data/v2/BTCUSDT-bookTicker-2024-03-28.csv \\
        --column 6 --from 1711612500000 --to 1711613100000 --output /tmp/btc_bbo_settle.csv

Reading is streamed and gzip is chosen by suffix, so a slice of a 1.2 GB file is one pass and no memory. The
column is 1-based, which is how a venue numbers its own columns. A row whose column is missing or not an
integer stops the run instead of being dropped: a filter that silently skips rows it cannot read is how a slice
ends up quietly short of the settlement it was cut for.
"""

import argparse
import csv
import gzip
from pathlib import Path


def open_text(path):
    if str(path).endswith(".gz"):
        return gzip.open(path, "rt", newline="")
    return open(path, newline="")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--column", type=int, required=True,
                        help="1-based column holding the integer to compare")
    parser.add_argument("--from", dest="start", type=int, required=True)
    parser.add_argument("--to", dest="end", type=int, required=True)
    parser.add_argument("--output", type=Path, help="omit to count the window without writing it")
    args = parser.parse_args()
    if args.column < 1:
        parser.error("--column is 1-based")
    if args.end < args.start:
        parser.error("--to must not be before --from")

    index = args.column - 1
    rows_read = 0
    rows_written = 0
    first_kept = None
    last_kept = None
    with open_text(args.input) as source:
        reader = csv.reader(source)
        try:
            header = next(reader)
        except StopIteration:
            parser.error(f"{args.input} is empty")
        if len(header) < args.column:
            parser.error(f"{args.input} has {len(header)} columns, so column {args.column} does not exist")

        out = None
        writer = None
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            out = open(args.output, "w", newline="")
            writer = csv.writer(out, lineterminator="\n")
            writer.writerow(header)
        try:
            for row in reader:
                rows_read += 1
                if len(row) < args.column:
                    parser.error(f"{args.input} row {rows_read + 1} is short of column {args.column}")
                try:
                    value = int(row[index])
                except ValueError:
                    parser.error(f"{args.input} row {rows_read + 1} column {args.column} is not an integer: "
                                 f"{row[index]!r}")
                if args.start <= value <= args.end:
                    rows_written += 1
                    first_kept = value if first_kept is None else first_kept
                    last_kept = value
                    if writer is not None:
                        writer.writerow(row)
        finally:
            if out is not None:
                out.close()
    print(f"rows_read={rows_read} rows_written={rows_written}")
    if first_kept is not None:
        print(f"first_kept={first_kept} last_kept={last_kept}")
    if args.output is not None:
        print(f"output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
