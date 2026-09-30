#!/usr/bin/env python3
"""Fetch one day of OKX tick trades from the public history endpoint, and slice one symbol out of it.

Answers what the public route can and cannot do, for the second venue this repository is heading towards:

* `module=1` is trades, `2` candles, `3` funding rates, `11` margin borrow rates, and the endpoint answers
  without an API key. There is no order book module; that dataset is only offered by the web portal.
* The endpoint returns a CDN zip per day, one CSV per zip, all instruments in one file, so one day of swaps is
  about 70 MB compressed and about 600 MB of CSV. This streams the member and writes only the requested symbol.
* **The daily file is cut on the UTC+8 day**, so the file named 2023-03-01 starts at 2023-02-28 16:00 UTC. That is
  the kind of fact that turns into a silently shifted day, so it is written here and printed on every run.
* Columns are `instrument_name,trade_id,side,price,size,created_time`, the timestamp is milliseconds, and `size`
  is in contracts, so a linear swap of contract value 0.01 needs that multiplication before it is a base amount.

Usage:
    python3 tools/fetch_okx_trades.py --date 2023-03-01 --symbol BTC-USDT-SWAP --output data/v2/okx_...
    python3 tools/fetch_okx_trades.py --date 2023-03-01 --symbol BTC-USDT-SWAP --zip /tmp/already-downloaded.zip
"""

import argparse
import csv
import io
import json
import sys
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

HISTORY_URL = "https://www.okx.com/api/v5/public/market-data-history"
# The endpoint answers a plain HTTP client but refuses the bare Python user agent, so it is set explicitly.
HEADERS = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) toyquant-history/1.0"}
# The file day is UTC+8, which is eight hours ahead of the timestamps inside it.
FILE_DAY_OFFSET = timedelta(hours=8)


def instrument_type(symbol):
    if symbol.endswith("-SWAP"):
        return "SWAP"
    if symbol.count("-") == 1:
        return "SPOT"
    raise ValueError(f"cannot tell the OKX instrument type of {symbol!r}")


def day_bounds(date):
    """Start and end of the file day in milliseconds, which is midnight UTC+8 rather than UTC."""
    start = datetime.strptime(date, "%Y-%m-%d").replace(tzinfo=timezone.utc) - FILE_DAY_OFFSET
    return int(start.timestamp() * 1000), int((start + timedelta(days=1)).timestamp() * 1000)


def find_url(symbol, start_ms, end_ms):
    query = urllib.parse.urlencode(
        {
            "module": 1,
            "instType": instrument_type(symbol),
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
            # The groups come back newest first, so the day is found by its stamp rather than by position.
            for group in groups:
                if int(group.get("dateTs", 0)) == start_ms:
                    return group["url"], group.get("filename", "")
            raise RuntimeError(
                f"the archive lists {len(groups)} file(s) for that range but none starts at the requested day")
    raise RuntimeError("no daily file for that date; the archive starts in September 2021 for trades")


def download(url, path):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=600) as response:
        with path.open("wb") as target:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                target.write(chunk)


def is_readable_zip(path):
    """A killed download leaves a partial file that exists and is not a zip, so it is checked before use."""
    try:
        with zipfile.ZipFile(path) as archive:
            return bool(archive.namelist())
    except (OSError, zipfile.BadZipFile):
        return False


def slice_symbol(archive_path, symbol, output, max_rows):
    """Stream the single member and keep one symbol, so a 600 MB CSV is never unpacked to disk."""
    written = 0
    first_ts = None
    last_ts = None
    with zipfile.ZipFile(archive_path) as archive:
        members = [name for name in archive.namelist() if name.endswith(".csv")]
        if len(members) != 1:
            raise RuntimeError(f"expected one CSV member, found {len(members)}")
        with archive.open(members[0]) as source:
            text = io.TextIOWrapper(source, encoding="utf-8", errors="replace", newline="")
            reader = csv.reader(text)
            header = next(reader)[:6]
            if header != ["instrument_name", "trade_id", "side", "price", "size", "created_time"]:
                raise RuntimeError(f"unexpected columns: {header}")
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("w", newline="", encoding="utf-8") as target:
                writer = csv.writer(target)
                writer.writerow(header)
                for row in reader:
                    if len(row) < 6 or row[0] != symbol:
                        continue
                    writer.writerow(row[:6])
                    written += 1
                    stamp = int(row[5])
                    first_ts = stamp if first_ts is None else first_ts
                    last_ts = stamp
                    if max_rows is not None and written >= max_rows:
                        break
    return written, first_ts, last_ts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", required=True, help="file day, for example 2023-03-01, which is midnight UTC+8")
    parser.add_argument("--symbol", required=True, help="for example BTC-USDT-SWAP or BTC-USDT")
    parser.add_argument("--output", type=Path, help="defaults to data/v2/okx_trades_DATE_SYMBOL.csv")
    parser.add_argument("--zip", type=Path, dest="zip_path", help="reuse a zip that is already on disk")
    parser.add_argument("--max-rows", type=int, help="stop after this many matching rows")
    args = parser.parse_args()
    if args.max_rows is not None and args.max_rows <= 0:
        parser.error("--max-rows must be positive")

    start_ms, end_ms = day_bounds(args.date)
    output = args.output or Path(f"data/v2/okx_trades_{args.date}_{args.symbol}.csv")

    try:
        archive = args.zip_path
        if archive is None:
            url, filename = find_url(args.symbol, start_ms, end_ms)
            print(f"url={url}")
            archive = Path("/tmp") / filename
            if archive.exists() and not is_readable_zip(archive):
                print(f"discarding an incomplete download: {archive}")
                archive.unlink()
            if not archive.exists():
                download(url, archive)
            if not is_readable_zip(archive):
                raise RuntimeError(f"the download is not a readable zip: {archive}")
            print(f"zip={archive} bytes={archive.stat().st_size}")
        written, first_ts, last_ts = slice_symbol(archive, args.symbol, output, args.max_rows)
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        parser.error(str(error))

    print(f"symbol={args.symbol} rows_written={written}")
    day_start = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc) - FILE_DAY_OFFSET
    print(f"file_day={args.date} starts at {day_start:%Y-%m-%d %H:%M} UTC, because the file day is UTC+8")
    if first_ts is not None:
        first = datetime.fromtimestamp(first_ts / 1000, tz=timezone.utc)
        last = datetime.fromtimestamp(last_ts / 1000, tz=timezone.utc)
        print(f"timestamp_unit=milliseconds range={first:%Y-%m-%d %H:%M:%S.%f} .. {last:%Y-%m-%d %H:%M:%S.%f} UTC")
    print(f"output={output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

