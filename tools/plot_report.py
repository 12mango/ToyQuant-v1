#!/usr/bin/env python3
"""Create a static ToyQuant simulation report from CSV outputs."""

import argparse
import csv
import io
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


COLORS = {
    "market": "#243447",
    "buy": "#168aad",
    "sell": "#e76f51",
    "inventory": "#6a4c93",
    "muted": "#657786",
    "panel": "#f5f7fa",
}


def read_rows(path):
    text = path.read_text()
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.strip():
            if line.lstrip().startswith("#"):
                lines[index] = line.replace("#", "", 1).lstrip()
            break
    return list(csv.DictReader(io.StringIO("\n".join(lines))))


def parse_args():
    parser = argparse.ArgumentParser(description="Plot a ToyQuant simulation report")
    parser.add_argument("--ticks", required=True, help="Tick scenario CSV")
    parser.add_argument("--orders", required=True, help="Runtime orders CSV")
    parser.add_argument("--trades", required=True, help="Runtime trades CSV")
    parser.add_argument("--output", required=True, help="Output PNG path")
    return parser.parse_args()


def build_report(tick_rows, order_rows, trade_rows, output):
    if not tick_rows:
        raise ValueError("tick file contains no rows")

    tick_rows.sort(key=lambda row: int(row["ts"]))
    order_rows.sort(key=lambda row: int(row["ts"]))
    trade_rows.sort(key=lambda row: int(row["ts"]))

    start_ts = int(tick_rows[0]["ts"])
    tick_x = [(int(row["ts"]) - start_ts) / 1000.0 for row in tick_rows]
    prices = [float(row["price"]) for row in tick_rows]

    buy_orders = [row for row in order_rows if row["side"] == "B"]
    sell_orders = [row for row in order_rows if row["side"] == "S"]
    buy_x = [(int(row["ts"]) - start_ts) / 1000.0 for row in buy_orders]
    sell_x = [(int(row["ts"]) - start_ts) / 1000.0 for row in sell_orders]

    trade_x = [(int(row["ts"]) - start_ts) / 1000.0 for row in trade_rows]
    trade_prices = [float(row["price"]) for row in trade_rows]
    trade_quantities = [int(row["quantity"]) for row in trade_rows]

    inventory = 0
    inventory_x = []
    inventory_y = []
    for row in trade_rows:
        quantity = int(row["quantity"])
        inventory += quantity if row["side"] == "B" else -quantity
        inventory_x.append((int(row["ts"]) - start_ts) / 1000.0)
        inventory_y.append(inventory)

    submitted_quantity = sum(int(row["quantity"]) for row in order_rows)
    traded_quantity = sum(trade_quantities)
    fill_rate = traded_quantity / submitted_quantity if submitted_quantity else 0.0
    order_ids = {row["order_id"] for row in order_rows}
    active_trade_ids = {row["order_id"] for row in trade_rows}
    unexecuted_orders = max(len(order_ids - active_trade_ids), 0)

    fig = plt.figure(figsize=(13, 8), facecolor="white")
    layout = fig.add_gridspec(2, 2, height_ratios=(1.55, 1), hspace=0.30, wspace=0.18)
    price_ax = fig.add_subplot(layout[0, :])
    inventory_ax = fig.add_subplot(layout[1, 0])
    summary_ax = fig.add_subplot(layout[1, 1])

    price_ax.plot(tick_x, prices, color=COLORS["market"], linewidth=2, label="Market price")
    if buy_orders:
        price_ax.scatter(buy_x, [float(row["price"]) for row in buy_orders],
                         s=18, alpha=0.65, color=COLORS["buy"], label="Buy quotes")
    if sell_orders:
        price_ax.scatter(sell_x, [float(row["price"]) for row in sell_orders],
                         s=18, alpha=0.65, color=COLORS["sell"], label="Sell quotes")
    if trade_rows:
        price_ax.scatter(trade_x, trade_prices, s=[max(18, q / 2) for q in trade_quantities],
                         color="#f4a261", edgecolors="white", linewidth=0.7,
                         label="Executions", zorder=5)
    price_ax.set_title("Market, quotes, and executions", loc="left", fontweight="bold")
    price_ax.set_ylabel("Price")
    price_ax.legend(frameon=False, ncol=4, loc="upper left")
    price_ax.grid(axis="y", alpha=0.18)

    inventory_ax.step(inventory_x, inventory_y, where="post", color=COLORS["inventory"], linewidth=2)
    inventory_ax.axhline(0, color=COLORS["muted"], linewidth=0.8)
    inventory_ax.set_title("Net inventory after executions", loc="left", fontweight="bold")
    inventory_ax.set_xlabel("Seconds from first tick")
    inventory_ax.set_ylabel("Net position")
    inventory_ax.grid(axis="y", alpha=0.18)
    inventory_ax.xaxis.set_major_locator(MaxNLocator(5))

    summary_ax.set_facecolor(COLORS["panel"])
    summary_ax.axis("off")
    symbol = tick_rows[0].get("symbol", "")
    summary_ax.text(0.06, 0.90, f"{symbol}  |  simulation summary", fontsize=13, fontweight="bold",
                    transform=summary_ax.transAxes, color=COLORS["market"])
    metrics = [
        ("Ticks", f"{len(tick_rows):,}"),
        ("Submitted orders", f"{len(order_rows):,}"),
        ("Executions", f"{len(trade_rows):,}"),
        ("Fill rate", f"{fill_rate:.1%}"),
        ("Orders without execution", f"{unexecuted_orders:,}"),
        ("Final net position", f"{inventory:+,}"),
    ]
    for index, (label, value) in enumerate(metrics):
        column = index % 2
        row = index // 2
        x = 0.07 + column * 0.48
        y = 0.68 - row * 0.22
        summary_ax.text(x, y, label, fontsize=9, color=COLORS["muted"], transform=summary_ax.transAxes)
        summary_ax.text(x, y - 0.09, value, fontsize=16, fontweight="bold",
                        color=COLORS["market"], transform=summary_ax.transAxes)

    fig.suptitle("ToyQuant Simulation Report", x=0.07, ha="left", fontsize=19, fontweight="bold")
    fig.text(0.07, 0.935, "Price action, strategy quotes, executions, and inventory",
             fontsize=10, color=COLORS["muted"])
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=160, bbox_inches="tight")
    plt.close(fig)


def main():
    args = parse_args()
    tick_rows = read_rows(Path(args.ticks))
    order_rows = read_rows(Path(args.orders))
    trade_rows = read_rows(Path(args.trades))
    build_report(tick_rows, order_rows, trade_rows, Path(args.output))
    print(f"created report -> {args.output}")


if __name__ == "__main__":
    main()