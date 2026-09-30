#!/usr/bin/env python3
"""Describe an unknown market data file before a reader for it is written.

A new source is a new set of assumptions, and the expensive ones fail silently: a timestamp in microseconds read
as milliseconds, a price column that is still a string, or an order book file that is incremental when a snapshot
was expected. None of those raise an error later. They produce a book that looks reasonable and is wrong, which is
the failure mode docs/ROADMAP.md names when it requires a reconciliation step before a source is trusted.

This prints what a file actually contains over its first rows: delimiter, header, column count, inferred types and
ranges, which integer columns look like timestamps and in what unit, whether they move forward, and the top of book
implied by the first row when a column carries a JSON array of levels. It is the anatomy step of accepting a new
source, and it runs before any reader exists.

Usage:
    python3 tools/inspect_source.py FILE [FILE ...] [--rows 200]
"""

import argparse
import csv
import gzip
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

DELIMITERS = [",", ";", "\t", "|"]
# Digit counts rather than ranges, so the guess reads as a rule instead of a table of magic numbers.
TIMESTAMP_UNITS = [(10, "seconds"), (13, "milliseconds"), (16, "microseconds"), (19, "nanoseconds")]
SECONDS_PER = {"seconds": 1, "milliseconds": 1e3, "microseconds": 1e6, "nanoseconds": 1e9}


def open_text(path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", newline="", encoding="utf-8-sig", errors="replace")
    return path.open("rt", newline="", encoding="utf-8-sig", errors="replace")


def sniff_delimiter(header):
    counts = {delimiter: header.count(delimiter) for delimiter in DELIMITERS}
    best = max(counts, key=lambda delimiter: counts[delimiter])
    return best if counts[best] > 0 else ","


def is_integer(text):
    try:
        int(text)
        return True
    except ValueError:
        return False


def is_number(text):
    try:
        float(text)
        return True
    except ValueError:
        return False


def timestamp_unit(text):
    """Unit implied by the digit count of a positive integer, or None when it is not one."""
    if not is_integer(text) or text.startswith("-"):
        return None
    digits = len(text.lstrip("0") or "0")
    for length, unit in TIMESTAMP_UNITS:
        if digits == length:
            return unit
    return None


def looks_like_timestamp(name, value):
    """A timestamp column by name, and by digit count only when the name does not look like an identifier.

    The digit count alone is not enough: `agg_trade_id` in a Binance aggregate trade file is a ten digit integer
    and would otherwise be reported as a timestamp in seconds, which is exactly the plausible-looking wrong answer
    this tool exists to prevent.
    """
    lowered = name.lower()
    if "id" in lowered:
        return None
    if any(hint in lowered for hint in ("time", "timestamp", "ts", "date")):
        return timestamp_unit(value) or "unknown"
    return timestamp_unit(value)


def as_utc(raw, unit):
    moment = datetime.fromtimestamp(int(raw) / SECONDS_PER[unit], tz=timezone.utc)
    return moment.strftime("%Y-%m-%d %H:%M:%S.%f")


def json_levels(text):
    """Levels from a JSON array column, as a list of lists, or None when the shape is something else."""
    if not text.startswith("["):
        return None
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return None
    if not isinstance(parsed, list) or not parsed:
        return None
    if not isinstance(parsed[0], list) or not parsed[0]:
        return None
    return parsed


def compression_of(path):
    """A truncated download reads as a prefix of the file, so it is worth knowing before interpreting rows."""
    if path.suffix != ".gz":
        return "plain"
    try:
        with gzip.open(path, "rb") as probe:
            while probe.read(1 << 20):
                pass
        return "gzip, complete"
    except (OSError, EOFError):
        return "gzip, TRUNCATED"


def read_rows(path, max_rows):
    """(delimiter, names, has_header, rows) for the first max_rows data rows."""
    with open_text(path) as source:
        first = source.readline()
        if not first:
            return None
        delimiter = sniff_delimiter(first.rstrip("\n"))
        fields = next(csv.reader([first.rstrip("\n")], delimiter=delimiter))
        # A header is present when at least one field cannot be a number, which no data row manages.
        has_header = any(not is_number(field) for field in fields)
        names = fields if has_header else [f"column_{index}" for index in range(len(fields))]
        reader = csv.reader(source, delimiter=delimiter)
        rows = list(fields for _, fields in zip(range(max_rows), reader))
        if not has_header:
            rows = [fields] + rows
        return delimiter, names, has_header, rows[:max_rows]


def summarize(name, values):
    if not values:
        print(f"  {name}: types=none")
        return
    types = set()
    numbers = []
    monotonic = True
    previous = None
    for value in values:
        if value == "":
            types.add("empty")
        elif is_integer(value):
            types.add("integer")
            numbers.append(int(value))
            if previous is not None and int(value) < previous:
                monotonic = False
            previous = int(value)
        elif is_number(value):
            types.add("float")
        else:
            types.add("string")
    span = f" min={min(numbers)} max={max(numbers)}" if numbers else ""
    print(f"  {name}: types={','.join(sorted(types)) or 'none'}{span} sample={values[0][:60]!r}")
    unit = looks_like_timestamp(name, values[0])
    if unit:
        if unit == "unknown" or not numbers:
            print(f"    timestamp unit={unit}, range not shown")
        else:
            print(f"    timestamp unit={unit} monotonic={'yes' if monotonic else 'no'}")
            print(f"    range {as_utc(min(numbers), unit)} .. {as_utc(max(numbers), unit)} UTC")
    levels = json_levels(values[0])
    if levels:
        print(f"    json levels={len(levels)} first_level={levels[0]}")


def inspect(path, max_rows):
    print(f"=== {path} ===")
    if not path.exists():
        print("missing: this path does not exist")
        print("status=warning reason=missing-file")
        return 1
    print(f"bytes={path.stat().st_size} compression={compression_of(path)}")

    read = read_rows(path, max_rows)
    if read is None:
        print("status=warning reason=empty-file")
        return 1
    delimiter, names, has_header, rows = read
    print(f"delimiter={delimiter!r} header={'present' if has_header else 'absent'} columns={len(names)} rows_read={len(rows)}")
    print(f"names={','.join(names)}")
    print("column summary over the rows read:")
    for index, name in enumerate(names):
        summarize(name, [row[index] for row in rows if index < len(row)])

    # The first row is enough to reconcile a top of book independently of any reader.
    level_columns = []
    for index, name in enumerate(names):
        for row in rows[:1]:
            if index < len(row) and json_levels(row[index]):
                level_columns.append((name, json_levels(row[index])))
    if level_columns:
        print("json level columns, taken from the first row:")
        for name, levels in level_columns:
            print(f"  {name}: levels={len(levels)} first_level={levels[0]}")
        if len(level_columns) == 2:
            (_, levels_a), (_, levels_b) = level_columns
            if float(levels_a[0][0]) < float(levels_b[0][0]):
                bid, ask = levels_a, levels_b
            else:
                bid, ask = levels_b, levels_a
            print(f"  implied_top_of_book bid={float(bid[0][0])} ask={float(ask[0][0])} "
                  f"spread={float(ask[0][0]) - float(bid[0][0])}")
        else:
            print("  status=warning reason=spread-needs-two-level-columns")

    print("status=ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--rows", type=int, default=200, help="rows to describe each file from, default 200")
    args = parser.parse_args()
    if args.rows <= 0:
        parser.error("--rows must be positive")

    status = 0
    for path in args.files:
        if inspect(path, args.rows) != 0:
            status = 1
        print()
    return status


if __name__ == "__main__":
    sys.exit(main())
