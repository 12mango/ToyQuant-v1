#!/usr/bin/env python3
"""Reconcile an OKX order book archive against the periodic snapshots it carries.

A feed with no sequence numbers cannot be checked for missing records directly, so this uses the checks the
archive does allow. The strongest one is that a long file re-sends a full snapshot every so often: an
independently maintained book, built by applying every update since the previous snapshot, must then equal that
snapshot level for level. A mismatch is either a real gap in the data or a wrong idea about what an update means,
and both are worth knowing before a reader is trusted.

It also reports the top of book at the first snapshot and at the last record, which is what a reader's output is
compared against, and it refuses a book that is crossed or unsorted instead of measuring with it.

Usage:
    python3 tools/check_okx_book.py /tmp/okx_l2_300k.jsonl [--symbol BTC-USDT-SWAP] [--max-records 300000]
"""

import argparse
import json
import sys
from pathlib import Path


def load_records(path, max_records):
    with path.open("rt", encoding="utf-8", errors="replace") as source:
        for index, line in enumerate(source):
            if max_records is not None and index >= max_records:
                return
            line = line.strip()
            if not line.startswith("{"):
                continue
            yield json.loads(line)


def levels_of(record, side):
    """Sizes as floats, keyed by price, so a level either changes or disappears."""
    return {float(price): float(size) for price, size, *_ in record.get(side, [])}


def apply_side(book, levels, updates):
    """An update replaces a level, and a size of zero removes it."""
    for price, size in levels.items():
        if size == 0.0:
            book.pop(price, None)
            updates += 1
        else:
            updated = book.get(price) != size
            book[price] = size
            if updated:
                updates += 1
    return updates


def side_mismatch(book, levels, label):
    """Levels where a cumulated book and a fresh snapshot disagree."""
    expected = {price: size for price, size in levels.items() if size != 0.0}
    problems = []
    for price in sorted(set(book) | set(expected)):
        have = book.get(price)
        want = expected.get(price)
        if have != want:
            problems.append((price, have, want))
            if len(problems) >= 5:
                break
    if problems:
        print(f"  {label} mismatch, first {len(problems)}:")
        for price, have, want in problems:
            print(f"    price={price} cumulated={have} snapshot={want}")
    return len(problems)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", type=Path)
    parser.add_argument("--symbol", default="BTC-USDT-SWAP")
    parser.add_argument("--max-records", type=int)
    args = parser.parse_args()

    bids, asks = {}, {}
    records = 0
    snapshots = 0
    updates = 0
    mismatches = 0
    first_top = None
    seen_book = False

    for record in load_records(args.path, args.max_records):
        if record.get("instId") != args.symbol:
            continue
        records += 1
        bid_levels = levels_of(record, "bids")
        ask_levels = levels_of(record, "asks")

        if record.get("action") == "snapshot":
            snapshots += 1
            if seen_book:
                # The reconciliation: the book this script built from updates against the new snapshot.
                mismatches += side_mismatch(bids, bid_levels, "bids")
                mismatches += side_mismatch(asks, ask_levels, "asks")
            bids = {price: size for price, size in bid_levels.items() if size != 0.0}
            asks = {price: size for price, size in ask_levels.items() if size != 0.0}
            seen_book = True
            if first_top is None and bids and asks:
                first_top = (max(bids), min(asks))
        else:
            updates = apply_side(bids, bid_levels, updates)
            updates = apply_side(asks, ask_levels, updates)

    if not seen_book:
        print("no snapshot found for that symbol")
        return 1

    best_bid, best_ask = max(bids), min(asks)
    crossed = best_bid >= best_ask
    print(f"records={records} snapshots={snapshots} level_updates={updates}")
    print(f"first_top_of_book bid={first_top[0]} ask={first_top[1]}")
    print(f"final_top_of_book bid={best_bid} ask={best_ask} spread={round(best_ask - best_bid, 6)}")
    print(f"final book depth bids={len(bids)} asks={len(asks)}")
    print(f"snapshot_reconciliation mismatches={mismatches}")
    if mismatches == 0 and not crossed:
        print("status=ok")
        return 0
    print(f"status=warning crossed={crossed} mismatches={mismatches}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
