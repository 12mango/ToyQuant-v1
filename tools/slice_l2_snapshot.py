#!/usr/bin/env python3
import argparse
import csv
import gzip
from pathlib import Path


def slice_snapshot(input_path, output_path, start_ts, end_ts, max_rows, every):
    if input_path.resolve() == output_path.resolve():
        raise ValueError("input and output paths must be different")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    rows_seen = 0
    rows_matched = 0
    rows_written = 0
    first_ts = None
    last_ts = None

    opener = gzip.open if input_path.suffix == ".gz" else Path.open
    with opener(input_path, "rt", newline="", encoding="utf-8-sig", errors="replace") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames:
            raise ValueError("input CSV is missing a header")
        if "timestamp" not in reader.fieldnames:
            raise ValueError("input CSV is missing the timestamp column")

        with output_path.open("w", newline="", encoding="utf-8") as target:
            writer = csv.DictWriter(target, fieldnames=reader.fieldnames)
            writer.writeheader()

            for row in reader:
                rows_seen += 1
                try:
                    timestamp = int(row["timestamp"])
                except (TypeError, ValueError) as error:
                    raise ValueError(f"invalid timestamp at input row {rows_seen + 1}") from error

                if start_ts is not None and timestamp < start_ts:
                    continue
                if end_ts is not None and timestamp > end_ts:
                    continue

                rows_matched += 1
                if (rows_matched - 1) % every != 0:
                    continue

                writer.writerow(row)
                rows_written += 1
                first_ts = timestamp if first_ts is None else first_ts
                last_ts = timestamp
                if max_rows is not None and rows_written >= max_rows:
                    break

    return rows_seen, rows_matched, rows_written, first_ts, last_ts


def main():
    parser = argparse.ArgumentParser(
        description="Stream a bounded sample or time window from an L2 snapshot CSV"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start-ts", type=int, help="inclusive timestamp in source units")
    parser.add_argument("--end-ts", type=int, help="inclusive timestamp in source units")
    parser.add_argument("--max-rows", type=int, help="maximum number of output data rows")
    parser.add_argument("--every", type=int, default=1, help="keep every Nth matching row")
    args = parser.parse_args()

    if args.start_ts is not None and args.end_ts is not None and args.start_ts > args.end_ts:
        parser.error("--start-ts must not be greater than --end-ts")
    if args.max_rows is not None and args.max_rows <= 0:
        parser.error("--max-rows must be positive")
    if args.every <= 0:
        parser.error("--every must be positive")

    try:
        stats = slice_snapshot(
            args.input,
            args.output,
            args.start_ts,
            args.end_ts,
            args.max_rows,
            args.every,
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))

    rows_seen, rows_matched, rows_written, first_ts, last_ts = stats
    print(f"source={args.input}")
    print(f"output={args.output}")
    print(f"rows_seen={rows_seen} rows_matched={rows_matched} rows_written={rows_written}")
    print(f"timestamp_range={first_ts}..{last_ts}")


if __name__ == "__main__":
    main()