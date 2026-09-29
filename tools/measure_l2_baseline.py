#!/usr/bin/env python3
"""Run a reproducible L2 replay timing baseline without touching runtime outputs by default."""

import argparse
import hashlib
import subprocess
import tempfile
import time
from pathlib import Path

from benchmark_l2_strategies import open_text, slice_csv


def digest_output(output: str) -> str:
    return hashlib.sha256(output.encode("utf-8")).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/toy_quant"))
    parser.add_argument("--trades", type=Path, required=True)
    parser.add_argument("--depth", type=Path, required=True)
    parser.add_argument("--symbol", default="BTC-PERPETUAL")
    parser.add_argument("--start-ts", type=int, required=True)
    parser.add_argument("--end-ts", type=int, required=True)
    parser.add_argument("--strategy", default="active_l2")
    parser.add_argument("--quantity-scale", type=int, default=1)
    parser.add_argument("--queue-model", choices=("conservative", "prorata", "optimistic"),
                        default="conservative")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--depth-every", type=int, default=1)
    parser.add_argument("--fast-validation", action="store_true")
    parser.add_argument("--with-output", action="store_true",
                        help="allow the replay to write data/runtime and logs outputs")
    args = parser.parse_args()

    if args.start_ts >= args.end_ts:
        parser.error("--start-ts must be less than --end-ts")
    if args.runs <= 0 or args.depth_every <= 0:
        parser.error("--runs and --depth-every must be positive")
    if not args.binary.is_file():
        parser.error(f"binary does not exist: {args.binary}")

    with tempfile.TemporaryDirectory(prefix="toyquant_l2_baseline_") as temp_name:
        temp_dir = Path(temp_name)
        trades_path = temp_dir / "trades.csv"
        depth_path = temp_dir / "depth.csv"
        trade_stats = slice_csv(args.trades, trades_path, args.start_ts, args.end_ts)
        depth_stats = slice_csv(args.depth, depth_path, args.start_ts, args.end_ts,
                                every=args.depth_every)
        command = [
            str(args.binary),
            "l2_replay",
            str(trades_path),
            str(depth_path),
            args.symbol,
            "0",
            args.strategy,
            str(args.quantity_scale),
            args.queue_model,
        ]
        if not args.with_output:
            command.append("--no-output")
        if args.fast_validation:
            command.append("--fast-validation")

        timings = []
        output_hashes = []
        for _ in range(args.runs):
            started = time.perf_counter()
            completed = subprocess.run(command, check=True, text=True,
                                       capture_output=True)
            timings.append(time.perf_counter() - started)
            output_hashes.append(digest_output(completed.stdout + completed.stderr))

    print(f"trades={trade_stats['rows']} depth={depth_stats['rows']}")
    print("seconds=" + " ".join(f"{value:.6f}" for value in timings))
    print(f"min={min(timings):.6f} median={sorted(timings)[len(timings) // 2]:.6f} "
          f"max={max(timings):.6f}")
    print(f"outputs_identical={len(set(output_hashes)) == 1}")
    print(f"output_sha256={output_hashes[0]}")


if __name__ == "__main__":
    main()
