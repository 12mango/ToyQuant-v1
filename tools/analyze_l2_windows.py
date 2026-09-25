#!/usr/bin/env python3
import argparse
import csv
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from benchmark_l2_strategies import (  # noqa: E402
    PRIMARY_STRATEGIES,
    count_overlap,
    open_text,
    run_strategy,
)


def split_csv(input_path, output_dir, prefix, start_ts, end_ts, window_us, every):
    writers = {}
    handles = {}
    counts = {}
    written_counts = {}
    try:
        with open_text(input_path) as source:
            reader = csv.DictReader(source)
            if not reader.fieldnames or "timestamp" not in reader.fieldnames:
                raise ValueError(f"{input_path} is missing a timestamp header")
            for row in reader:
                timestamp = int(row["timestamp"])
                if timestamp < start_ts:
                    continue
                if timestamp > end_ts:
                    break
                window = (timestamp - start_ts) // window_us
                counts.setdefault(window, 0)
                counts[window] += 1
                if (counts[window] - 1) % every != 0:
                    continue
                if window not in writers:
                    path = output_dir / f"{prefix}_{window}.csv"
                    handle = path.open("w", newline="", encoding="utf-8")
                    handles[window] = handle
                    writer = csv.DictWriter(handle, fieldnames=reader.fieldnames)
                    writer.writeheader()
                    writers[window] = writer
                writers[window].writerow(row)
                written_counts[window] = written_counts.get(window, 0) + 1
    finally:
        for handle in handles.values():
            handle.close()
    return written_counts


def main():
    parser = argparse.ArgumentParser(description="Analyze L2 strategy performance by time window")
    parser.add_argument("--binary", type=Path, default=Path("build/toy_quant"))
    parser.add_argument("--trades", type=Path, required=True)
    parser.add_argument("--depth", type=Path, required=True)
    parser.add_argument("--symbol", default="BTC-PERPETUAL")
    parser.add_argument("--start-ts", type=int, required=True)
    parser.add_argument("--end-ts", type=int, required=True)
    parser.add_argument("--window-hours", type=int, default=1)
    parser.add_argument("--window-minutes", type=int,
                        help="window width in minutes; overrides --window-hours")
    parser.add_argument("--depth-every", type=int, default=20)
    parser.add_argument("--csv", type=Path, required=True)
    args = parser.parse_args()

    if args.start_ts >= args.end_ts:
        parser.error("--start-ts must be less than --end-ts")
    if args.window_hours <= 0 or (args.window_minutes is not None and args.window_minutes <= 0) or args.depth_every <= 0:
        parser.error("window width and --depth-every must be positive")

    window_us = ((args.window_minutes * 60) if args.window_minutes is not None
                 else (args.window_hours * 60 * 60)) * 1_000_000
    columns = [
        "window",
        "window_start_ts",
        "window_end_ts",
        "strategy",
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
        "price_refresh_count",
        "age_refresh_count",
        "risk_pause_count",
        "working_orders",
        "fill_rate",
        "cancel_rate",
        "realized",
        "unrealized",
        "equity",
        "gross_pnl",
        "net_pnl",
        "fees",
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

    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="toyquant_l2_windows_") as temp_name:
        temp_dir = Path(temp_name)
        trade_counts = split_csv(
            args.trades, temp_dir, "trades", args.start_ts, args.end_ts, window_us, 1)
        depth_counts = split_csv(
            args.depth, temp_dir, "depth", args.start_ts, args.end_ts, window_us,
            args.depth_every)
        windows = sorted(set(trade_counts) & set(depth_counts))
        with args.csv.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=columns)
            writer.writeheader()
            for window in windows:
                trade_path = temp_dir / f"trades_{window}.csv"
                depth_path = temp_dir / f"depth_{window}.csv"
                overlap = count_overlap(trade_path, depth_path)
                for strategy in PRIMARY_STRATEGIES:
                    result = run_strategy(args.binary, trade_path, depth_path, args.symbol, strategy, 1)
                    row = {column: result.get(column, "") for column in columns}
                    row.update({
                        "window": window,
                        "window_start_ts": args.start_ts + window * window_us,
                        "window_end_ts": args.start_ts + (window + 1) * window_us,
                        "strategy": strategy,
                        "depth_every": args.depth_every,
                        "input_trades": trade_counts[window],
                        "input_depth": depth_counts[window],
                        "overlap_trades": overlap,
                    })
                    writer.writerow(row)
                    print(window, strategy, "net_pnl=", result.get("net_pnl"),
                          "fills=", result.get("fills"),
                          "markout20=", result.get("avg_markout_20"),
                          "against_flow=", result.get("against_flow"))


if __name__ == "__main__":
    main()
