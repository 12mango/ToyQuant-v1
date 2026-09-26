#!/usr/bin/env python3
"""Compare reconstructed snapshot top levels with an incremental L2 feed."""

import argparse
import csv
import gzip
from pathlib import Path


def open_text(path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", newline="", encoding="utf-8-sig", errors="replace")
    return path.open("r", newline="", encoding="utf-8-sig", errors="replace")


def incremental_batches(path):
    with open_text(path) as source:
        reader = csv.DictReader(source)
        batch = []
        batch_local = None
        for row in reader:
            local_ts = int(row["local_timestamp"])
            if batch and local_ts != batch_local:
                yield batch
                batch = []
            batch.append(row)
            batch_local = local_ts
        if batch:
            yield batch


def apply_batch(batch, bids, asks, started, snapshot_active):
    contains_snapshot = any(row["is_snapshot"].lower() in {"true", "1"} for row in batch)
    reset = contains_snapshot and not snapshot_active
    if reset:
        bids.clear()
        asks.clear()
    for row in batch:
        levels = bids if row["side"] == "bid" else asks
        price = float(row["price"])
        amount = int(round(float(row["amount"])))
        if amount == 0:
            levels.pop(price, None)
        else:
            levels[price] = amount
    return started or contains_snapshot, contains_snapshot


def snapshot_top(row, levels):
    bids = []
    asks = []
    for index in range(levels):
        bid_price = row.get(f"bids[{index}].price", "")
        bid_amount = row.get(f"bids[{index}].amount", "")
        ask_price = row.get(f"asks[{index}].price", "")
        ask_amount = row.get(f"asks[{index}].amount", "")
        if bid_price and bid_amount:
            bids.append((float(bid_price), int(round(float(bid_amount)))))
        if ask_price and ask_amount:
            asks.append((float(ask_price), int(round(float(ask_amount)))))
    return bids, asks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--incremental", type=Path, required=True)
    parser.add_argument("--sample-every", type=int, default=1000)
    parser.add_argument("--levels", type=int, default=5)
    args = parser.parse_args()
    if args.sample_every <= 0 or args.levels <= 0:
        parser.error("--sample-every and --levels must be positive")

    batches = incremental_batches(args.incremental)
    next_batch = next(batches, None)
    bids = {}
    asks = {}
    started = False
    snapshot_active = False
    sampled = matched = 0
    max_bid_price_diff = max_ask_price_diff = 0.0
    max_bid_amount_diff = max_ask_amount_diff = 0
    snapshot_rows = 0

    with open_text(args.snapshot) as source:
        for row in csv.DictReader(source):
            snapshot_rows += 1
            snapshot_ts = int(row["timestamp"])
            while next_batch is not None and int(next_batch[0]["timestamp"]) <= snapshot_ts:
                started, snapshot_active = apply_batch(
                    next_batch, bids, asks, started, snapshot_active)
                next_batch = next(batches, None)
            if snapshot_rows % args.sample_every != 0 or not started:
                continue
            expected_bids, expected_asks = snapshot_top(row, args.levels)
            actual_bids = sorted(bids.items(), reverse=True)[:args.levels]
            actual_asks = sorted(asks.items())[:args.levels]
            sampled += 1
            if expected_bids == actual_bids and expected_asks == actual_asks:
                matched += 1
            for expected, actual, side in (
                (expected_bids, actual_bids, "bid"),
                (expected_asks, actual_asks, "ask"),
            ):
                for index in range(min(len(expected), len(actual))):
                    price_diff = abs(expected[index][0] - actual[index][0])
                    amount_diff = abs(expected[index][1] - actual[index][1])
                    if side == "bid":
                        max_bid_price_diff = max(max_bid_price_diff, price_diff)
                        max_bid_amount_diff = max(max_bid_amount_diff, amount_diff)
                    else:
                        max_ask_price_diff = max(max_ask_price_diff, price_diff)
                        max_ask_amount_diff = max(max_ask_amount_diff, amount_diff)

    print(f"snapshot_rows={snapshot_rows}")
    print(f"sampled_rows={sampled}")
    print(f"exact_top_matches={matched}")
    print(f"exact_match_rate={matched / sampled if sampled else 0.0}")
    print(f"max_bid_price_diff={max_bid_price_diff}")
    print(f"max_ask_price_diff={max_ask_price_diff}")
    print(f"max_bid_amount_diff={max_bid_amount_diff}")
    print(f"max_ask_amount_diff={max_ask_amount_diff}")


if __name__ == "__main__":
    main()
