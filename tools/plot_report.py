#!/usr/bin/env python3
"""Create a static ToyQuant strategy report with a PRO DARK TERMINAL theme."""

import argparse
import bisect
import csv
import io
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch
from matplotlib.ticker import FormatStrFormatter, MaxNLocator

TICK_SIZE = 0.00001
DEFAULT_MAX_INVENTORY = 1000

# 📊 顶级量化终端配色方案 (Terminal Dark Theme)
COLORS = {
    "bg": "#0B0E14",             # 极深黑蓝色背景
    "panel": "#131822",          # 子图面板背景
    "mid": "#E0E6ED",            # 冰白 Reference Mid Line
    "buy": "#00E676",            # 霓虹绿 (Buy Band & Trades)
    "sell": "#FF5252",           # 鲜艳红 (Sell Band & Trades)
    "inventory": "#00E5FF",      # 电光蓝 (Net Inventory Line)
    "limit": "#FF9100",          # 琥珀黄 (Risk Limits)
    "grid": "#212B3B",           # 极浅网格线
    "text": "#E2E8F0",           # 主文字
    "muted": "#64748B",          # 次要/暗色文字
    "card_bg": "#131822",        # 右侧 Dashboard 卡片背景
    "card_border": "#212B3B",    # Dashboard 边框
    "alert": "#FF5252",          # 越界警示区
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
    parser = argparse.ArgumentParser(description="Plot a ToyQuant strategy report")
    parser.add_argument("--ticks", required=True, help="Tick scenario CSV")
    parser.add_argument("--orders", required=True, help="Runtime orders CSV")
    parser.add_argument("--trades", required=True, help="Runtime trades CSV")
    parser.add_argument("--output", required=True, help="Output PNG path")
    parser.add_argument("--max-inventory", type=int, default=DEFAULT_MAX_INVENTORY)
    return parser.parse_args()


def relative_time(row, start_ts):
    return (int(row["ts"]) - start_ts) / 1000.0


def aggregate_quotes(rows, side):
    levels = defaultdict(list)
    for row in rows:
        levels[int(row["ts"])].append(float(row["price"]))
    timestamps = sorted(levels)
    return timestamps, [min(levels[ts]) for ts in timestamps], [max(levels[ts]) for ts in timestamps]


def nearest_reference_price(timestamp, tick_timestamps, tick_prices):
    index = bisect.bisect_left(tick_timestamps, timestamp)
    if index == 0:
        return tick_prices[0]
    if index == len(tick_timestamps):
        return tick_prices[-1]
    previous = index - 1
    if timestamp - tick_timestamps[previous] <= tick_timestamps[index] - timestamp:
        return tick_prices[previous]
    return tick_prices[index]


def distance_series(rows, tick_timestamps, tick_prices):
    levels = defaultdict(list)
    for row in rows:
        reference = nearest_reference_price(int(row["ts"]), tick_timestamps, tick_prices)
        levels[int(row["ts"])].append((float(row["price"]) - reference) / TICK_SIZE)
    timestamps = sorted(levels)
    return timestamps, [min(levels[ts]) for ts in timestamps], [max(levels[ts]) for ts in timestamps]


def build_report(tick_rows, order_rows, trade_rows, output, max_inventory):
    if not tick_rows:
        raise ValueError("tick file contains no rows")

    tick_rows.sort(key=lambda row: int(row["ts"]))
    order_rows.sort(key=lambda row: int(row["ts"]))
    trade_rows.sort(key=lambda row: int(row["ts"]))
    start_ts = int(tick_rows[0]["ts"])

    tick_x = [relative_time(row, start_ts) for row in tick_rows]
    tick_timestamps = [int(row["ts"]) for row in tick_rows]
    tick_prices = [float(row["price"]) for row in tick_rows]
    buy_orders = [row for row in order_rows if row["side"] == "B"]
    sell_orders = [row for row in order_rows if row["side"] == "S"]
    buy_ts, buy_low, buy_high = aggregate_quotes(buy_orders, "B")
    sell_ts, sell_low, sell_high = aggregate_quotes(sell_orders, "S")
    buy_x = [(ts - start_ts) / 1000.0 for ts in buy_ts]
    sell_x = [(ts - start_ts) / 1000.0 for ts in sell_ts]

    trade_x = [relative_time(row, start_ts) for row in trade_rows]
    trade_prices = [float(row["price"]) for row in trade_rows]
    trade_quantities = [int(row["quantity"]) for row in trade_rows]
    buy_trades = [row for row in trade_rows if row["side"] == "B"]
    sell_trades = [row for row in trade_rows if row["side"] == "S"]

    mid_x = tick_x
    mid_prices = tick_prices

    inventory = 0
    inventory_x = []
    inventory_y = []
    for row in trade_rows:
        quantity = int(row["quantity"])
        inventory += quantity if row["side"] == "B" else -quantity
        inventory_x.append(relative_time(row, start_ts))
        inventory_y.append(inventory)

    buy_distance_ts, buy_distance_low, buy_distance_high = distance_series(
        buy_orders, tick_timestamps, tick_prices)
    sell_distance_ts, sell_distance_low, sell_distance_high = distance_series(
        sell_orders, tick_timestamps, tick_prices)
    distance_buy_x = [(ts - start_ts) / 1000.0 for ts in buy_distance_ts]
    distance_sell_x = [(ts - start_ts) / 1000.0 for ts in sell_distance_ts]
    distance_peak = max(
        [abs(value) for values in (buy_distance_low, buy_distance_high,
                                   sell_distance_low, sell_distance_high) for value in values],
        default=1,
    )

    submitted_quantity = sum(int(row["quantity"]) for row in order_rows)
    traded_quantity = sum(trade_quantities)
    buy_trade_quantity = sum(int(row["quantity"]) for row in buy_trades)
    sell_trade_quantity = sum(int(row["quantity"]) for row in sell_trades)
    max_abs_inventory = max((abs(value) for value in inventory_y), default=0)
    fill_rate = traded_quantity / submitted_quantity if submitted_quantity else 0.0
    order_ids = {row["order_id"] for row in order_rows}
    traded_ids = {row["order_id"] for row in trade_rows}
    unexecuted_orders = len(order_ids - traded_ids)

    plt.style.use("dark_background")
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["DejaVu Sans", "Helvetica", "Arial"],
        "axes.titleweight": "bold"
    })

    fig = plt.figure(figsize=(15, 10), facecolor=COLORS["bg"])
    layout = fig.add_gridspec(3, 5, width_ratios=(1, 1, 1, 1, 0.92),
                              height_ratios=(2.0, 1.15, 1.15), hspace=0.35, wspace=0.18)

    price_ax = fig.add_subplot(layout[0, :4], facecolor=COLORS["panel"])
    distance_ax = fig.add_subplot(layout[1, :4], sharex=price_ax, facecolor=COLORS["panel"])
    inventory_ax = fig.add_subplot(layout[2, :4], sharex=price_ax, facecolor=COLORS["panel"])
    kpi_ax = fig.add_subplot(layout[:, 4], facecolor=COLORS["card_bg"])

    # --- 1. Price Chart ---
    price_ax.plot(mid_x, mid_prices, color=COLORS["mid"], linewidth=1.2, alpha=0.85, label="Reference Mid", zorder=2)
    if buy_x:
        price_ax.fill_between(buy_x, buy_low, buy_high, color=COLORS["buy"], alpha=0.15)
        price_ax.plot(buy_x, buy_low, color=COLORS["buy"], linewidth=1.0, alpha=0.8)
        price_ax.plot(buy_x, buy_high, color=COLORS["buy"], linewidth=1.0, alpha=0.8)
    if sell_x:
        price_ax.fill_between(sell_x, sell_low, sell_high, color=COLORS["sell"], alpha=0.15)
        price_ax.plot(sell_x, sell_low, color=COLORS["sell"], linewidth=1.0, alpha=0.8)
        price_ax.plot(sell_x, sell_high, color=COLORS["sell"], linewidth=1.0, alpha=0.8)
    if buy_trades:
        price_ax.scatter([relative_time(row, start_ts) for row in buy_trades],
                         [float(row["price"]) for row in buy_trades],
                         s=[max(28, int(row["quantity"]) / 1.8) for row in buy_trades],
                         marker="^", color=COLORS["buy"], edgecolors=COLORS["bg"], linewidth=0.6, zorder=5)
    if sell_trades:
        price_ax.scatter([relative_time(row, start_ts) for row in sell_trades],
                         [float(row["price"]) for row in sell_trades],
                         s=[max(28, int(row["quantity"]) / 1.8) for row in sell_trades],
                         marker="v", color=COLORS["sell"], edgecolors=COLORS["bg"], linewidth=0.6, zorder=5)

    price_ax.set_title("Quotes & Executions", loc="left", color=COLORS["text"], pad=12, fontsize=12)
    price_ax.set_ylabel("Price", color=COLORS["muted"], fontsize=9)
    price_ax.yaxis.set_major_formatter(FormatStrFormatter("%.5f"))

    # --- 2. Distance Chart ---
    if distance_buy_x:
        distance_ax.fill_between(distance_buy_x, buy_distance_low, buy_distance_high, color=COLORS["buy"], alpha=0.18)
        distance_ax.plot(distance_buy_x, buy_distance_low, color=COLORS["buy"], linewidth=1.0)
        distance_ax.plot(distance_buy_x, buy_distance_high, color=COLORS["buy"], linewidth=1.0)
    if distance_sell_x:
        distance_ax.fill_between(distance_sell_x, sell_distance_low, sell_distance_high, color=COLORS["sell"], alpha=0.18)
        distance_ax.plot(distance_sell_x, sell_distance_low, color=COLORS["sell"], linewidth=1.0)
        distance_ax.plot(distance_sell_x, sell_distance_high, color=COLORS["sell"], linewidth=1.0)
    distance_ax.axhline(0, color=COLORS["muted"], linewidth=0.8, linestyle="--")

    # [修正纵轴爆框问题] 设置充足的比例留白
    distance_ax.set_ylim(-max(18, distance_peak * 1.3), max(18, distance_peak * 1.3))
    distance_ax.set_title("Quote Distance to Reference Mid", loc="left", color=COLORS["text"], fontsize=12)
    distance_ax.set_ylabel("Distance (ticks)", color=COLORS["muted"], fontsize=9)

    # --- 3. Inventory Chart ---
    inventory_ax.step(inventory_x, inventory_y, where="post", color=COLORS["inventory"], linewidth=1.8)
    inventory_ax.axhline(0, color=COLORS["muted"], linewidth=0.8, linestyle="--")
    inventory_ax.axhline(max_inventory, color=COLORS["limit"], linestyle=":", linewidth=1.2)
    inventory_ax.axhline(-max_inventory, color=COLORS["limit"], linestyle=":", linewidth=1.2)

    # [修正风险警告色块对称]
    inventory_ax.axhspan(max_inventory, max_inventory * 1.5, color=COLORS["alert"], alpha=0.12)
    inventory_ax.axhspan(-max_inventory * 1.5, -max_inventory, color=COLORS["alert"], alpha=0.12)

    inventory_ax.set_title("Net Inventory Profile", loc="left", color=COLORS["text"], fontsize=12)
    inventory_ax.set_ylabel("Position", color=COLORS["muted"], fontsize=9)
    inventory_ax.set_xlabel("Seconds from First Tick", color=COLORS["muted"], fontsize=9)

    # 统一网格与边框美化
    for axis in (price_ax, distance_ax, inventory_ax):
        axis.grid(True, color=COLORS["grid"], linestyle=":", linewidth=0.6, alpha=0.7)
        axis.spines["top"].set_visible(False)
        axis.spines["right"].set_visible(False)
        axis.spines["left"].set_color(COLORS["card_border"])
        axis.spines["bottom"].set_color(COLORS["card_border"])
        axis.tick_params(colors=COLORS["muted"], labelsize=8)
        axis.xaxis.set_major_locator(MaxNLocator(8))

    # --- 4. Right Sidebar: RUN SUMMARY ---
    kpi_ax.spines[:].set_color(COLORS["card_border"])
    kpi_ax.spines[:].set_linewidth(1.0)
    kpi_ax.set_xticks([])
    kpi_ax.set_yticks([])
    kpi_ax.set_xlim(0, 1)
    kpi_ax.set_ylim(0, 1)

    kpi_ax.text(0.12, 0.94, "RUN SUMMARY", fontsize=11, fontweight="bold",
                color=COLORS["muted"], transform=kpi_ax.transAxes)

    kpi_metrics = [
        ("Ticks", f"{len(tick_rows):,}"),
        ("Orders", f"{len(order_rows):,}"),
        ("Executions", f"{len(trade_rows):,}"),
        ("Traded Qty", f"{traded_quantity:,}"),
        ("Buy / Sell Qty", f"{buy_trade_quantity:,} / {sell_trade_quantity:,}"),
        ("Fill Rate", f"{fill_rate:.1%}"),
        ("Final Inventory", f"{inventory:+,}"),
        ("Peak |Inventory|", f"{max_abs_inventory:,}"),
        ("Unexecuted Orders", f"{unexecuted_orders:,}"),
    ]
    for index, (label, value) in enumerate(kpi_metrics):
        y = 0.86 - index * 0.088
        kpi_ax.text(0.10, y, label, fontsize=8, color=COLORS["muted"], transform=kpi_ax.transAxes)
        kpi_ax.text(0.10, y - 0.038, value, fontsize=11, fontweight="bold",
                    color=COLORS["text"], transform=kpi_ax.transAxes)

    # 顶部标题 & 图例排版
    fig.subplots_adjust(top=0.82, bottom=0.08, left=0.08, right=0.97)
    fig.suptitle("ToyQuant Strategy Report", x=0.08, y=0.96, ha="left", fontsize=20,
                 fontweight="bold", color=COLORS["text"])

    legend_elements = [
        Line2D([0], [0], color=COLORS["mid"], lw=1.5, label="Reference Mid"),
        Patch(facecolor=COLORS["buy"], alpha=0.3, label="Buy Quote Band"),
        Patch(facecolor=COLORS["sell"], alpha=0.3, label="Sell Quote Band"),
        Line2D([0], [0], marker="^", color="w", markerfacecolor=COLORS["buy"], markersize=7, label="Buy Fill"),
        Line2D([0], [0], marker="v", color="w", markerfacecolor=COLORS["sell"], markersize=7, label="Sell Fill"),
    ]
    fig.legend(handles=legend_elements, frameon=False, ncol=5, loc="upper left",
               bbox_to_anchor=(0.08, 0.915), borderaxespad=0, fontsize=8, labelcolor=COLORS["text"])

    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=200, bbox_inches="tight", facecolor=COLORS["bg"])
    plt.close(fig)


def main():
    args = parse_args()
    build_report(read_rows(Path(args.ticks)), read_rows(Path(args.orders)),
                 read_rows(Path(args.trades)), Path(args.output), args.max_inventory)
    print(f"Created report -> {args.output}")


if __name__ == "__main__":
    main()