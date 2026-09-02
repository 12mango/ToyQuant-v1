#!/usr/bin/env python3
import argparse
import csv
import random

TICK_SIZE = 0.00001
BASE_PRICE = 1.18300
SYMBOL = "EURUSD"
TS_START = 1_759_080_000_000
TS_STEP = 100


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
    raise ValueError(f"unknown scenario: {scenario}")


def generate(scenario, count, seed):
    rng = random.Random(seed)
    rows = []
    for index in range(count):
        rows.append([
            TS_START + index * TS_STEP,
            SYMBOL,
            f"{price_for(scenario, index, count, rng):.5f}",
            rng.randint(20, 200),
            "B" if rng.random() < 0.5 else "S",
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
    parser.add_argument("scenario", choices=("flat", "uptrend", "downtrend", "shock", "random", "all"),
                        nargs="?", default="all")
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-dir", default="data")
    args = parser.parse_args()

    scenarios = ("flat", "uptrend", "downtrend", "shock", "random")
    if args.scenario != "all":
        scenarios = (args.scenario,)

    for offset, scenario in enumerate(scenarios):
        rows = generate(scenario, args.count, args.seed + offset)
        write_csv(f"{args.output_dir}/{scenario}_ticks.csv", rows)


if __name__ == "__main__":
    main()
