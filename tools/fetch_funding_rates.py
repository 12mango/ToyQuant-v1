#!/usr/bin/env python3
"""Fetch one instrument's funding history from a free source, as a CSV a replay can read.

The second input the carry line needs, after the flow mechanism: funding, the basis at roll and a
dividend all settle on a clock, so the rate has to arrive as a series with timestamps rather than as a
single number, and the accounting applies one settlement at a time as the replay passes it.

* **OKX** (`--source okx`) serves funding rates from the same public history endpoint as its trades,
  under `module=3`. The archive is one zip per day for *every* swap on the venue, so the requested
  instrument is sliced out of it. Its file day is midnight **UTC+8**, so the file named 2026-01-07
  begins at 2026-01-06 16:00 UTC, which is a settlement instant and not the start of the UTC day the
  book archive for the same date covers. That is how a day shifts silently, so this prints it.
* **Binance Vision** (`--source binance`) publishes funding rates as a **monthly** zip per symbol
  (`fundingRate/` has no daily folder, unlike `premiumIndexKlines`, which is a different series), with
  one row per settlement and the interval declared in the row. `calc_time` is on the UTC day it names.

Both are free and keyless. The output keeps the source's own columns, so the reader in
`src/market/funding_rate.cpp` parses the file the venue published rather than a translation of it.

Usage:
    python3 tools/fetch_funding_rates.py --source okx --date 2026-01-07 --symbol BTC-USDT-SWAP
    python3 tools/fetch_funding_rates.py --source okx --from 2024-03-01 --to 2024-03-31 --symbol BTC-USDT-SWAP
    python3 tools/fetch_funding_rates.py --source binance --month 2024-03 --symbol BTCUSDT

A range of OKX file days is cut and concatenated into one series, because a carry needs two histories
that cover the same window and the Binance archive is monthly while the OKX one is daily.
"""

import argparse
import csv
import io
import json
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

HISTORY_URL = "https://www.okx.com/api/v5/public/market-data-history"
BINANCE_URL = "https://data.binance.vision/data/futures/um/monthly/fundingRate"
# The endpoint answers a plain HTTP client but refuses the bare Python user agent.
HEADERS = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) toyquant-history/1.0"}
# The OKX file day is UTC+8, eight hours ahead of the timestamps inside it.
FILE_DAY_OFFSET = timedelta(hours=8)
OKX_HEADER = "instrument_name,funding_rate,funding_time"
BINANCE_HEADER = "calc_time,funding_interval_hours,last_funding_rate"


def day_bounds(date):
    """Start and end of the OKX file day in milliseconds, which is midnight UTC+8 rather than UTC."""
    start = datetime.strptime(date, "%Y-%m-%d").replace(tzinfo=timezone.utc) - FILE_DAY_OFFSET
    return int(start.timestamp() * 1000), int((start + timedelta(days=1)).timestamp() * 1000)


def find_url(symbol, date, start_ms, end_ms):
    query = urllib.parse.urlencode(
        {
            "module": 3,
            "instType": "SWAP",
            "instId": symbol,
            "dateAggrType": "daily",
            "begin": start_ms,
            "end": end_ms,
        }
    )
    with urllib.request.urlopen(urllib.request.Request(f"{HISTORY_URL}?{query}", headers=HEADERS),
                                timeout=60) as response:
        payload = json.load(response)
    if payload.get("code") != "0":
        raise RuntimeError(f"the history endpoint said {payload.get('code')} {payload.get('msg')!r}")
    for entry in payload.get("data", []):
        for detail in entry.get("details", []):
            # The range starts live on the detail, while each file inside it is named by dateTs.
            if int(detail.get("dateRangeStart", 0)) != start_ms:
                continue
            groups = detail.get("groupDetails", [])
            if not groups:
                raise RuntimeError("the archive lists that day but returned no file for it")
            # A day's detail can list the file for the next day as well, and the groups carry no range of
            # their own, so the file named for the requested day is the one asked for. Taking the first
            # group instead returns the next day's file and shifts every settlement by eight hours.
            wanted = f"-{date}.zip"
            for group in groups:
                if group.get("filename", "").endswith(wanted):
                    return group["url"], group["filename"]
            raise RuntimeError(f"{date} is listed but no file is named for it: "
                               f"{', '.join(g.get('filename', '') for g in groups)}")
    raise RuntimeError(f"no file covers {start_ms}..{end_ms}")


def download(url, target):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=300) as response:
        target.write_bytes(response.read())


def is_readable_zip(path):
    try:
        with zipfile.ZipFile(path) as archive:
            return bool(archive.namelist())
    except (OSError, zipfile.BadZipFile):
        return False


def ensure_zip(url, archive):
    if archive.exists() and not is_readable_zip(archive):
        print(f"discarding an incomplete download: {archive}")
        archive.unlink()
    if not archive.exists():
        download(url, archive)
    if not is_readable_zip(archive):
        raise RuntimeError(f"the download is not a readable zip: {archive}")
    print(f"zip={archive} bytes={archive.stat().st_size}")


def only_member(archive):
    members = [name for name in archive.namelist() if name.endswith(".csv")]
    if len(members) != 1:
        raise RuntimeError(f"expected one CSV member, found {len(members)}")
    return members[0]


def fetch_okx_day(date, symbol, zip_path=None):
    """One OKX file day's rows for one instrument, downloading the archive unless it is already there."""
    start_ms, end_ms = day_bounds(date)
    archive = zip_path
    if archive is None:
        url, filename = find_url(symbol, date, start_ms, end_ms)
        print(f"url={url}")
        archive = Path("/tmp") / filename
        ensure_zip(url, archive)
    with zipfile.ZipFile(archive) as zipped:
        member = only_member(zipped)
        with zipped.open(member) as source:
            text = io.TextIOWrapper(source, encoding="utf-8", errors="replace", newline="")
            reader = csv.reader(text)
            header = next(reader)
            if ",".join(header[:3]) != OKX_HEADER:
                raise RuntimeError(f"unexpected columns: {header}")
            rows = [row[:3] for row in reader if row and row[0] == symbol]
    return rows


def extract_binance(archive_path, output):
    """The Binance funding archive holds one symbol already, so its member is copied as it is."""
    with zipfile.ZipFile(archive_path) as archive:
        raw = archive.read(only_member(archive)).decode("utf-8", errors="replace")
    reader = csv.reader(io.StringIO(raw))
    header = next(reader)
    if ",".join(header) != BINANCE_HEADER:
        raise RuntimeError(f"unexpected columns: {header}")
    rows = [row for row in reader if row and row[0]]
    write_rows(output, header, rows)
    return rows


def write_rows(output, header, rows):
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as target:
        writer = csv.writer(target)
        writer.writerow(header)
        writer.writerows(rows)


def report(output, rows, timestamp_index, declared_interval=False):
    """The shape of the series, printed rather than left in a comment: the reader's numbers come from it."""
    stamps = sorted(int(row[timestamp_index]) for row in rows)
    print(f"points={len(rows)}")
    if not stamps:
        return
    first = datetime.fromtimestamp(stamps[0] / 1000, tz=timezone.utc)
    last = datetime.fromtimestamp(stamps[-1] / 1000, tz=timezone.utc)
    print(f"first={first:%Y-%m-%d %H:%M} UTC last={last:%Y-%m-%d %H:%M} UTC")
    print(f"span_hours={(stamps[-1] - stamps[0]) / 3600000:g}")
    if len(stamps) > 1:
        print(f"timestamp_interval_hours={(stamps[1] - stamps[0]) / 3600000:g}")
    if declared_interval:
        print(f"declared_interval_hours={','.join(sorted({row[1] for row in rows}))}")
    print(f"output={output}")


def okx_days(args):
    """The file days to fetch: one for --date, or every day of --from..--to."""
    if args.date:
        if args.start or args.end:
            raise ValueError("--date is a single day; --from and --to are a range, so pass one or the other")
        return [args.date]
    if not (args.start and args.end):
        raise ValueError("--source okx needs --date, or --from with --to")
    first = datetime.strptime(args.start, "%Y-%m-%d").date()
    last = datetime.strptime(args.end, "%Y-%m-%d").date()
    if last < first:
        raise ValueError("--to must not be before --from")
    days = []
    day = first
    while day <= last:
        days.append(day.strftime("%Y-%m-%d"))
        day += timedelta(days=1)
    return days


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, choices=("okx", "binance"))
    parser.add_argument("--symbol", required=True, help="for example BTC-USDT-SWAP or BTCUSDT")
    parser.add_argument("--date", help="one OKX file day, for example 2026-01-07, which is midnight UTC+8")
    parser.add_argument("--from", dest="start", help="first OKX file day of a range, with --to")
    parser.add_argument("--to", dest="end", help="last OKX file day of a range, with --from")
    parser.add_argument("--month", help="Binance month, for example 2024-03")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--zip", type=Path, dest="zip_path", help="reuse a zip that is already on disk")
    args = parser.parse_args()
    if args.source == "okx" and not (args.date or (args.start and args.end)):
        parser.error("--source okx needs --date, or --from with --to")
    if args.source == "binance" and not args.month:
        parser.error("--source binance needs --month")

    try:
        if args.source == "okx":
            days = okx_days(args)
            # Consecutive OKX file days tile without a gap and without an overlap: the file named D holds the
            # 16:00, 00:00 and 08:00 UTC settlements of its own UTC+8 day, and the next one picks up at
            # 16:00. Cutting on UTC+8 while the trades archive is cut on UTC is what makes a range worth
            # printing as it is written.
            if len(days) == 1:
                output = args.output or Path(f"data/v2/okx_funding_{days[0]}_{args.symbol}.csv")
            else:
                output = args.output or Path(
                    f"data/v2/okx_funding_{days[0]}_{days[-1]}_{args.symbol}.csv")
            rows = []
            for day in days:
                rows += fetch_okx_day(day, args.symbol, args.zip_path if len(days) == 1 else None)
            rows.sort(key=lambda row: int(row[2]))
            write_rows(output, OKX_HEADER.split(","), rows)
            first = datetime.strptime(days[0], "%Y-%m-%d").replace(tzinfo=timezone.utc) - FILE_DAY_OFFSET
            print(f"file_days={days[0]}..{days[-1]} the first starts at {first:%Y-%m-%d %H:%M} UTC, "
                  f"because the file day is UTC+8")
            report(output, rows, 2)
        else:
            output = args.output or Path(f"data/v2/{args.symbol}-fundingRate-{args.month}.csv")
            archive = args.zip_path
            if archive is None:
                url = f"{BINANCE_URL}/{args.symbol}/{args.symbol}-fundingRate-{args.month}.zip"
                print(f"url={url}")
                archive = Path("/tmp") / f"{args.symbol}-fundingRate-{args.month}.zip"
                ensure_zip(url, archive)
            rows = extract_binance(archive, output)
            print("file_day=UTC, because calc_time is on the UTC day the file names")
            report(output, rows, 0, declared_interval=True)
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


