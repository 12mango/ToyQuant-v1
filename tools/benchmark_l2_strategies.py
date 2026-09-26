#!/usr/bin/env python3
import argparse
import csv
import gzip
import re
import subprocess
import tempfile
from pathlib import Path


PRIMARY_STRATEGIES = (
    "passive_l2",
    "inventory_aware_l2",
    "flow_aware_l2",
    "active_l2",
)
EXPERIMENTAL_STRATEGIES = ("l2_depth", "l2_micro", "l2_flow")
EXECUTION_RE = re.compile(
    r"submitted_orders=(?P<orders>\d+).*?submitted_quantity=(?P<submitted_qty>\d+)"
    r".*?cancel_requests=(?P<cancels>\d+)"
    r".*?trade_reports=(?P<fills>\d+).*?fill_rate=(?P<fill_rate>[-+0-9.eE]+)"
    r".*?cancel_rate=(?P<cancel_rate>[-+0-9.eE]+).*?trade_report_quantity=(?P<filled_qty>\d+)"
    r".*?queue_ahead_consumed=(?P<queue_ahead_consumed>\d+)"
    r".*?buy_queue_ahead_levels_cleared=(?P<buy_queue_ahead_levels_cleared>\d+)"
    r".*?sell_queue_ahead_levels_cleared=(?P<sell_queue_ahead_levels_cleared>\d+)"
    r".*?buy_queue_from_quantity_changes=(?P<buy_queue_from_quantity_changes>\d+)"
    r".*?sell_queue_from_quantity_changes=(?P<sell_queue_from_quantity_changes>\d+)"
    r".*?working_orders=(?P<working_orders>\d+)"
)
PORTFOLIO_RE = re.compile(
    r"cash=(?P<cash>[-+0-9.eE]+).*?realized_pnl=(?P<realized>[-+0-9.eE]+)"
    r".*?unrealized_pnl=(?P<unrealized>[-+0-9.eE]+).*?equity=(?P<equity>[-+0-9.eE]+)"
    r".*?fees_paid=(?P<fees>[-+0-9.eE]+).*?maker_trade_count=(?P<maker>\d+)"
    r".*?taker_trade_count=(?P<taker>\d+)"
)
DATA_RE = re.compile(r"events=(?P<events>\d+) trades=(?P<trades>\d+) depth_snapshots=(?P<depth>\d+)")
STRATEGY_RE = re.compile(
    r"price_refresh_count=(?P<price_refresh_count>\d+)"
    r".*?age_refresh_count=(?P<age_refresh_count>\d+)"
    r".*?risk_pause_count=(?P<risk_pause_count>\d+)"
    r".*?buy_queue_consumed=(?P<buy_queue_consumed>\d+)"
    r".*?sell_queue_consumed=(?P<sell_queue_consumed>\d+)"
    r".*?buy_fill_count=(?P<buy_fill_count>\d+)"
    r".*?sell_fill_count=(?P<sell_fill_count>\d+)"
    r".*?buy_quote_count=(?P<buy_quote_count>\d+)"
    r".*?sell_quote_count=(?P<sell_quote_count>\d+)"
)
_INCREMENTAL_DEPTH_CACHE = {}


def row_timestamp(row):
    return int(row["timestamp"])


def open_text(path):
    return gzip.open(path, "rt", newline="", encoding="utf-8-sig", errors="replace") if path.suffix == ".gz" else path.open("r", newline="", encoding="utf-8-sig", errors="replace")


def slice_csv(input_path, output_path, start_ts, end_ts, max_rows=None, every=1):
    rows_seen = 0
    rows_written = 0
    first_ts = None
    last_ts = None
    with open_text(input_path) as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or "timestamp" not in reader.fieldnames:
            raise ValueError(f"{input_path} is missing a timestamp header")
        incremental = "is_snapshot" in reader.fieldnames and "local_timestamp" in reader.fieldnames
        baseline_path = output_path.with_suffix(output_path.suffix + ".baseline")
        baseline = None
        in_snapshot_batch = False
        baseline_written = False
        snapshot_started = False
        with output_path.open("w", newline="", encoding="utf-8") as target:
            writer = csv.DictWriter(target, fieldnames=reader.fieldnames)
            writer.writeheader()
            if incremental:
                baseline = baseline_path.open("w", newline="", encoding="utf-8")
                baseline_writer = csv.DictWriter(baseline, fieldnames=reader.fieldnames)
                baseline_writer.writeheader()

            def write_row(row):
                nonlocal rows_written, first_ts, last_ts
                writer.writerow(row)
                rows_written += 1
                ts = int(row["timestamp"])
                first_ts = ts if first_ts is None else first_ts
                last_ts = ts

            for row in reader:
                rows_seen += 1
                ts = int(row["timestamp"])
                if incremental:
                    is_snapshot = row["is_snapshot"].lower() in {"true", "1"}
                    if is_snapshot and not in_snapshot_batch:
                        in_snapshot_batch = True
                        snapshot_started = True
                        if not baseline_written:
                            baseline.close()
                            baseline = baseline_path.open("w", newline="", encoding="utf-8")
                            baseline_writer = csv.DictWriter(
                                baseline, fieldnames=reader.fieldnames)
                            baseline_writer.writeheader()
                    elif in_snapshot_batch:
                        in_snapshot_batch = False
                    if incremental and snapshot_started and not baseline_written and ts < start_ts:
                        baseline_writer.writerow(row)
                if ts < start_ts:
                    continue
                if ts > end_ts:
                    break
                if incremental and not baseline_written:
                    baseline.flush()
                    baseline.close()
                    with baseline_path.open("r", newline="", encoding="utf-8") as prefix:
                        for prefix_row in csv.DictReader(prefix):
                            write_row(prefix_row)
                    baseline_written = True
                if (rows_seen - 1) % every != 0:
                    continue
                write_row(row)
                if max_rows is not None and rows_written >= max_rows:
                    break
        if baseline is not None and not baseline.closed:
            baseline.close()
        if baseline_path.exists():
            baseline_path.unlink()
    return {"rows": rows_written, "first_ts": first_ts, "last_ts": last_ts}


def count_overlap(trades_path, depth_path):
    with depth_path.open(newline="") as depth_file:
        depth_rows = list(csv.DictReader(depth_file))
    with trades_path.open(newline="") as trades_file:
        trade_rows = list(csv.DictReader(trades_file))
    if not depth_rows or not trade_rows:
        return 0
    depth_start = int(depth_rows[0]["timestamp"])
    depth_end = int(depth_rows[-1]["timestamp"])
    return sum(1 for row in trade_rows if depth_start <= int(row["timestamp"]) <= depth_end)


def reconstruct_incremental_depth(path, levels=5):
    cached = _INCREMENTAL_DEPTH_CACHE.get(str(path))
    if cached is not None:
        return cached

    bids = {}
    asks = {}
    started = False
    snapshot_active = False
    timeline = []
    batch = []
    batch_local = None

    def emit(current):
        nonlocal started, snapshot_active
        if not current:
            return
        contains_snapshot = any(row["is_snapshot"].lower() in {"true", "1"}
                                for row in current)
        if contains_snapshot and not snapshot_active:
            bids.clear()
            asks.clear()
        snapshot_active = contains_snapshot
        for row in current:
            book = bids if row["side"] == "bid" else asks
            price = float(row["price"])
            amount = int(round(float(row["amount"])))
            if amount == 0:
                book.pop(price, None)
            else:
                book[price] = amount
        started = started or contains_snapshot
        if not started or not bids or not asks:
            return
        bid_levels = sorted(bids.items(), reverse=True)[:levels]
        ask_levels = sorted(asks.items())[:levels]
        if bid_levels[0][0] >= ask_levels[0][0]:
            return
        result = {"timestamp": current[-1]["timestamp"]}
        for index, (price, amount) in enumerate(bid_levels):
            result[f"bids[{index}].price"] = price
            result[f"bids[{index}].amount"] = amount
        for index, (price, amount) in enumerate(ask_levels):
            result[f"asks[{index}].price"] = price
            result[f"asks[{index}].amount"] = amount
        timeline.append(result)

    with open_text(path) as source:
        reader = csv.DictReader(source)
        for row in reader:
            local_ts = int(row["local_timestamp"])
            if batch and local_ts != batch_local:
                emit(batch)
                batch = []
            batch.append(row)
            batch_local = local_ts
        emit(batch)

    _INCREMENTAL_DEPTH_CACHE[str(path)] = timeline
    return timeline


def top_mid(depth_row):
    return (float(depth_row["bids[0].price"]) + float(depth_row["asks[0].price"])) / 2.0


def depth_imbalance(depth_row, levels=5):
    bid_depth = 0
    ask_depth = 0
    for level in range(levels):
        bid_key = f"bids[{level}].amount"
        ask_key = f"asks[{level}].amount"
        if bid_key not in depth_row or ask_key not in depth_row:
            break
        bid_depth += int(round(float(depth_row[bid_key])))
        ask_depth += int(round(float(depth_row[ask_key])))
    total_depth = bid_depth + ask_depth
    return 0.0 if total_depth == 0 else bid_depth / total_depth * 2.0 - 1.0


def trade_imbalance_before(trade_rows, timestamp, window=32):
    recent = []
    for row in trade_rows:
        if row_timestamp(row) >= timestamp:
            break
        recent.append(row)
        if len(recent) > window:
            recent.pop(0)
    buy_volume = sum(int(round(float(row["amount"]))) for row in recent if row["side"] == "buy")
    sell_volume = sum(int(round(float(row["amount"]))) for row in recent if row["side"] == "sell")
    total_volume = buy_volume + sell_volume
    return 0.0 if total_volume == 0 else buy_volume / total_volume * 2.0 - 1.0


def lower_bound_depth(depth_rows, timestamp):
    left = 0
    right = len(depth_rows)
    while left < right:
        mid = (left + right) // 2
        if row_timestamp(depth_rows[mid]) < timestamp:
            left = mid + 1
        else:
            right = mid
    return left


def summarize_fill_quality(trades_path, depth_path, orders_path=Path("data/runtime/orders.csv"), fills_path=Path("data/runtime/trades.csv")):
    with trades_path.open(newline="") as source:
        market_trades = list(csv.DictReader(source))
    with depth_path.open(newline="") as source:
        depth_rows = list(csv.DictReader(source))
    with orders_path.open(newline="") as source:
        orders = {row["order_id"]: row for row in csv.DictReader(row for row in source if not row.startswith("#"))}
    with fills_path.open(newline="") as source:
        fills = list(csv.DictReader(row for row in source if not row.startswith("#")))

    summary = {
        "avg_markout_5": 0.0,
        "avg_markout_20": 0.0,
        "buy_markout_5": 0.0,
        "sell_markout_5": 0.0,
        "buy_markout_20": 0.0,
        "sell_markout_20": 0.0,
        "avg_quote_age_us": 0.0,
        "against_depth": 0,
        "against_flow": 0,
        "net_position": 0,
        "max_abs_position": 0,
        "average_abs_position": 0.0,
        "inventory_sign_changes": 0,
    }
    if depth_rows and "bids[0].price" not in depth_rows[0]:
        depth_rows = reconstruct_incremental_depth(depth_path)
    if not fills or not depth_rows:
        return summary

    markout_5 = []
    markout_20 = []
    buy_markout_5 = []
    sell_markout_5 = []
    buy_markout_20 = []
    sell_markout_20 = []
    quote_ages = []
    position = 0
    absolute_positions = []
    previous_sign = 0
    for fill in fills:
        if not fill or fill.get("ts") in (None, "") or fill.get("price") in (None, ""):
            continue
        try:
            fill_ts = int(fill["ts"])
            fill_price = float(fill["price"])
            side = fill["side"]
            quantity = int(fill["quantity"])
        except (TypeError, ValueError):
            continue
        if side not in {"B", "S"}:
            continue
        position += quantity if side == "B" else -quantity
        absolute_positions.append(abs(position))
        current_sign = 1 if position > 0 else -1 if position < 0 else 0
        if previous_sign != 0 and current_sign != 0 and current_sign != previous_sign:
            summary["inventory_sign_changes"] += 1
        if current_sign != 0:
            previous_sign = current_sign
        summary["max_abs_position"] = max(summary["max_abs_position"], abs(position))
        depth_index = lower_bound_depth(depth_rows, fill_ts)
        if depth_index < len(depth_rows):
            fill_depth_imbalance = depth_imbalance(depth_rows[depth_index])
            if (side == "B" and fill_depth_imbalance < 0.0) or (side == "S" and fill_depth_imbalance > 0.0):
                summary["against_depth"] += 1
        fill_trade_imbalance = trade_imbalance_before(market_trades, fill_ts)
        if (side == "B" and fill_trade_imbalance < 0.0) or (side == "S" and fill_trade_imbalance > 0.0):
            summary["against_flow"] += 1
        for horizon, target in ((5, markout_5), (20, markout_20)):
            future_index = depth_index + horizon
            if future_index >= len(depth_rows):
                continue
            future_mid = top_mid(depth_rows[future_index])
            edge = future_mid - fill_price if side == "B" else fill_price - future_mid
            target.append(edge)
            if horizon == 5:
                (buy_markout_5 if side == "B" else sell_markout_5).append(edge)
            else:
                (buy_markout_20 if side == "B" else sell_markout_20).append(edge)
        order = orders.get(fill["order_id"])
        if order:
            quote_ages.append(fill_ts - int(order["ts"]))

    if markout_5:
        summary["avg_markout_5"] = sum(markout_5) / len(markout_5)
    if markout_20:
        summary["avg_markout_20"] = sum(markout_20) / len(markout_20)
    for key, values in (
        ("buy_markout_5", buy_markout_5),
        ("sell_markout_5", sell_markout_5),
        ("buy_markout_20", buy_markout_20),
        ("sell_markout_20", sell_markout_20),
    ):
        if values:
            summary[key] = sum(values) / len(values)
    if quote_ages:
        summary["avg_quote_age_us"] = sum(quote_ages) / len(quote_ages)
    summary["net_position"] = position
    if absolute_positions:
        summary["average_abs_position"] = sum(absolute_positions) / len(absolute_positions)
    return summary


def parse_run_output(output):
    result = {}
    for line in output.splitlines():
        if line.startswith("[L2 DATA]"):
            match = DATA_RE.search(line)
            if match:
                result.update({key: int(value) for key, value in match.groupdict().items()})
        elif line.startswith("[EXECUTION]"):
            match = EXECUTION_RE.search(line)
            if match:
                for key, value in match.groupdict().items():
                    result[key] = float(value) if "rate" in key else int(value)
        elif line.startswith("[PORTFOLIO]"):
            match = PORTFOLIO_RE.search(line)
            if match:
                for key, value in match.groupdict().items():
                    result[key] = int(value) if key in {"maker", "taker"} else float(value)
        elif line.startswith("[STRATEGY_METRICS]"):
            match = STRATEGY_RE.search(line)
            if match:
                result.update({key: int(value) for key, value in match.groupdict().items()})
    return result


def run_strategy(binary, trades_path, depth_path, symbol, strategy, quantity_scale, queue_model):
    command = [
        str(binary),
        "l2_replay",
        str(trades_path),
        str(depth_path),
        symbol,
        "0",
        strategy,
        str(quantity_scale),
        queue_model,
    ]
    completed = subprocess.run(command, check=True, text=True, capture_output=True)
    result = parse_run_output(completed.stdout)
    result.update(summarize_fill_quality(trades_path, depth_path))
    if "equity" in result:
        result["net_pnl"] = result["equity"] - 1000.0
    if "realized" in result and "unrealized" in result and "fees" in result:
        result["gross_pnl"] = result["realized"] + result["unrealized"] + result["fees"]
        result["fee_ratio"] = (
            result["fees"] / abs(result["gross_pnl"])
            if abs(result["gross_pnl"]) > 1e-12
            else 0.0
        )
    if "buy_quote_count" in result:
        result["buy_fill_probability"] = (
            result.get("buy_fill_count", 0) / result["buy_quote_count"]
            if result["buy_quote_count"] else 0.0
        )
        result["buy_queue_per_quote"] = (
            result.get("buy_queue_consumed", 0) / result["buy_quote_count"]
            if result["buy_quote_count"] else 0.0
        )
    if "sell_quote_count" in result:
        result["sell_fill_probability"] = (
            result.get("sell_fill_count", 0) / result["sell_quote_count"]
            if result["sell_quote_count"] else 0.0
        )
        result["sell_queue_per_quote"] = (
            result.get("sell_queue_consumed", 0) / result["sell_quote_count"]
            if result["sell_quote_count"] else 0.0
        )
    return result


def main():
    parser = argparse.ArgumentParser(description="Benchmark L2 strategy variants on an aligned window")
    parser.add_argument("--binary", type=Path, default=Path("build/toy_quant"))
    parser.add_argument("--trades", type=Path, required=True)
    parser.add_argument("--depth", type=Path, required=True)
    parser.add_argument("--symbol", default="BTC-PERPETUAL")
    parser.add_argument("--start-ts", type=int, required=True)
    parser.add_argument("--end-ts", type=int, required=True)
    parser.add_argument("--max-trades", type=int, help="optional cap on sliced trade rows")
    parser.add_argument("--max-depth", type=int, help="optional cap on sliced depth rows")
    parser.add_argument("--depth-every", type=int, default=1)
    parser.add_argument("--quantity-scale", type=int, default=1)
    parser.add_argument("--queue-model", choices=("conservative", "heuristic", "optimistic"),
                        default="conservative")
    parser.add_argument("--include-experimental", action="store_true", help="also run internal L2 signal aliases")
    parser.add_argument("--csv", type=Path, help="optional output CSV path")
    args = parser.parse_args()

    if args.start_ts > args.end_ts:
        parser.error("--start-ts must not exceed --end-ts")
    if args.depth_every <= 0:
        parser.error("--depth-every must be positive")

    with tempfile.TemporaryDirectory(prefix="toyquant_l2_bench_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        sliced_trades = temp_dir / "trades.csv"
        sliced_depth = temp_dir / "depth.csv"
        trade_stats = slice_csv(args.trades, sliced_trades, args.start_ts, args.end_ts, args.max_trades)
        depth_stats = slice_csv(args.depth, sliced_depth, args.start_ts, args.end_ts, args.max_depth, args.depth_every)
        overlap_trades = count_overlap(sliced_trades, sliced_depth)

        rows = []
        strategies = PRIMARY_STRATEGIES + (EXPERIMENTAL_STRATEGIES if args.include_experimental else ())
        for strategy in strategies:
            row = {"strategy": strategy}
            row.update(run_strategy(args.binary, sliced_trades, sliced_depth, args.symbol,
                                    strategy, args.quantity_scale, args.queue_model))
            row["input_trades"] = trade_stats["rows"]
            row["input_depth"] = depth_stats["rows"]
            row["overlap_trades"] = overlap_trades
            row["depth_every"] = args.depth_every
            row["queue_model"] = args.queue_model
            rows.append(row)

    columns = [
        "strategy",
        "queue_model",
        "depth_every",
        "input_trades",
        "input_depth",
        "overlap_trades",
        "orders",
        "submitted_qty",
        "cancels",
        "fills",
        "filled_qty",
        "queue_ahead_consumed",
        "buy_queue_ahead_levels_cleared",
        "sell_queue_ahead_levels_cleared",
        "buy_queue_from_quantity_changes",
        "sell_queue_from_quantity_changes",
        "price_refresh_count",
        "age_refresh_count",
        "risk_pause_count",
        "buy_queue_consumed",
        "sell_queue_consumed",
        "buy_fill_count",
        "sell_fill_count",
        "buy_quote_count",
        "sell_quote_count",
        "buy_fill_probability",
        "sell_fill_probability",
        "buy_queue_per_quote",
        "sell_queue_per_quote",
        "working_orders",
        "fill_rate",
        "cancel_rate",
        "realized",
        "unrealized",
        "equity",
        "gross_pnl",
        "net_pnl",
        "fees",
        "fee_ratio",
        "maker",
        "taker",
        "avg_markout_5",
        "avg_markout_20",
        "buy_markout_5",
        "sell_markout_5",
        "buy_markout_20",
        "sell_markout_20",
        "avg_quote_age_us",
        "against_depth",
        "against_flow",
        "net_position",
        "max_abs_position",
        "average_abs_position",
        "inventory_sign_changes",
    ]
    print("\t".join(columns))
    for row in rows:
        print("\t".join(str(row.get(column, "")) for column in columns))

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=columns)
            writer.writeheader()
            writer.writerows({column: row.get(column, "") for column in columns} for row in rows)


if __name__ == "__main__":
    main()
