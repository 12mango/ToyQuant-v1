#!/usr/bin/env python3
import argparse
import csv
import math
import random

TICK_SIZE = 0.00001
BASE_PRICE = 1.18300
SYMBOL = "EURUSD"
TS_START = 1_759_080_000_000
TS_STEP = 100


def smoothstep(value):
    value = max(0.0, min(1.0, value))
    return value * value * (3.0 - 2.0 * value)


def price_for(scenario, index, count, rng):
    if scenario == "flat":
        return BASE_PRICE + rng.choice((-2, -1, 0, 1, 2)) * TICK_SIZE
    if scenario == "uptrend":
        return BASE_PRICE + (index // 5) * TICK_SIZE + rng.choice((-1, 0, 1)) * TICK_SIZE
    if scenario == "downtrend":
        return BASE_PRICE - (index // 5) * TICK_SIZE + rng.choice((-1, 0, 1)) * TICK_SIZE
    if scenario == "shock":
        shock = 0 if index < count // 2 else 20
        return BASE_PRICE + (shock + rng.choice((-1, 0, 1))) * TICK_SIZE
    if scenario == "random":
        return BASE_PRICE + rng.randint(-50, 50) * TICK_SIZE
    if scenario == "synthetic":
        # A continuous three-regime path: quiet, gently rising, then retracing.
        # Smooth interpolation avoids artificial jumps at regime boundaries.
        progress = index / max(count - 1, 1)
        if progress < 0.25:
            trend = 0.0
        elif progress < 0.60:
            trend = 22.0 * smoothstep((progress - 0.25) / 0.35)
        else:
            trend = 22.0 * (1.0 - smoothstep((progress - 0.60) / 0.40))

        cycle = 2.5 * math.sin(progress * 8.0 * math.pi)
        noise = rng.choice((-1, 0, 0, 0, 1))
        return BASE_PRICE + (trend + cycle + noise) * TICK_SIZE
    raise ValueError(f"unknown scenario: {scenario}")


def generate(scenario, count, seed):
    rng = random.Random(seed)
    rows = []
    for index in range(count):
        price = price_for(scenario, index, count, rng)
        if index == 0:
            side = "B" if rng.random() < 0.5 else "S"
        else:
            previous_price = rows[-1][2]
            direction = price - float(previous_price)
            if abs(direction) < TICK_SIZE * 0.25:
                side = "B" if rng.random() < 0.5 else "S"
            else:
                # Keep direction informative without making it deterministic.
                expected_side = "B" if direction > 0 else "S"
                side = expected_side if rng.random() < 0.75 else ("S" if expected_side == "B" else "B")
        rows.append([
            TS_START + index * TS_STEP,
            SYMBOL,
            f"{price:.5f}",
            rng.randint(20, 200),
            side,
        ])
    return rows


def write_csv(path, rows):
    with open(path, "w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["ts", "symbol", "price", "size", "side"])
        writer.writerows(rows)
    print(f"generated {len(rows)} ticks -> {path}")


def main():
    parser = argparse.ArgumentParser(description="Generate reproducible HFT tick scenarios")
    parser.add_argument("scenario", choices=("flat", "uptrend", "downtrend", "shock", "random", "synthetic", "all"),
                        nargs="?", default="all")
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-dir", default="data/scenarios")
    args = parser.parse_args()

    scenarios = ("flat", "uptrend", "downtrend", "shock", "random", "synthetic")
    if args.scenario != "all":
        scenarios = (args.scenario,)

    for offset, scenario in enumerate(scenarios):
        rows = generate(scenario, args.count, args.seed + offset)
        write_csv(f"{args.output_dir}/{scenario}_ticks.csv", rows)


if __name__ == "__main__":
    main()
