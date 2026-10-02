#!/usr/bin/env python3
"""Rank instruments by the one ratio that survived four falsified markets: capture against the position's own move.

Four markets have been closed by this project, and each was closed by the same quotient rather than by a parameter:
the capture a fill can collect divided by the standard deviation of the position that fill leaves behind. MU's own
numbers are the worked example of failing it -- 941 USD of capture a window against 10,588 USD of position standard
deviation, 0.089 -- and the sweep that followed showed no quoting parameter moving that ratio by more than twenty
percent, which is why the venue and the instrument are what is left to choose.

This tool computes the same quotient on any top of book file the repository already owns, so that the next instrument
is picked by the measurement rather than by an impression, and it reports it in a form comparable across markets with
different ticks and multipliers:

    R_max = prints x (half spread - cost a unit) / sigma

the largest number of trade prints per fill that still covers one window's own standard deviation. It is a fill ratio,
so it is comparable to the 71 prints a fill the equity screen's fill model used: a series whose R_max is above that
reference can absorb a worse queue than the reference assumes, and one below it cannot.

The cost a unit is the account's schedule charged on the order instance the fill belongs to, because a cancel and
replace makes a new one, so the clip size is an argument rather than an assumption:

    cost a unit = max(min commission, fee a unit x clip) / clip

A share schedule's floor binds below 100 shares at 0.35 with a 0.0035 rate, and an option contract's binds below two
contracts, which is why both are run at two clips below.

Usage:
    python3 tools/venue_ratio.py --input 'data/v2/screen/*.csv' --clip 1,100 --label equities
    python3 tools/venue_ratio.py --input /tmp/tcbbo_picks2.csv --multiplier 100 --fee-per-unit 0.65 \\
        --min-commission 1.00 --clip 1,2 --label options
"""

import argparse
import csv
import glob
import statistics
import sys
from collections import defaultdict

REFERENCE = 71.0


def rows_of(path):
    """Every instrument in one top of book file, as (bid, ask, is a print) triples."""
    grouped = defaultdict(list)
    with open(path, newline="") as handle:
        reader = csv.DictReader(handle)
        names = set(reader.fieldnames or [])
        if not {"bid_px_00", "ask_px_00"} <= names:
            return grouped
        has_action = "action" in names
        for row in reader:
            try:
                bid, ask = float(row["bid_px_00"]), float(row["ask_px_00"])
            except (TypeError, ValueError):
                continue
            if bid <= 0.0 or ask <= 0.0:
                continue
            is_print = (row.get("action") == "T") if has_action else True
            grouped[(row.get("symbol") or path).strip()].append((bid, ask, is_print))
    return grouped


def measure(rows, multiplier):
    spreads = [ask - bid for bid, ask, _ in rows if ask > bid]
    if len(spreads) < 5:
        return None
    mids = [(bid + ask) / 2.0 for bid, ask, _ in rows]
    sigma = statistics.pstdev(mids) * multiplier
    if sigma <= 0.0:
        return None
    return {"prints": sum(1 for _, _, is_print in rows if is_print),
            "rows": len(rows),
            "spread": statistics.median(spreads),
            "half_usd": statistics.median(spreads) / 2.0 * multiplier,
            "premium": statistics.median(mids) * multiplier,
            "sigma": sigma}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, help="a file, a glob, or several separated by commas")
    parser.add_argument("--label", default="")
    parser.add_argument("--multiplier", type=float, default=1.0)
    parser.add_argument("--fee-per-unit", type=float, default=0.0035)
    parser.add_argument("--min-commission", type=float, default=0.35)
    parser.add_argument("--clip", default="1,100")
    parser.add_argument("--extra-per-unit", type=float, default=0.0,
                        help="per unit fees on every execution: clearing and the audit trail")
    parser.add_argument("--regulatory-rate", type=float, default=0.0,
                        help="fraction of the traded price charged on a sale, which is how the SEC fee works")
    parser.add_argument("--rebate-per-unit", type=float, default=0.0,
                        help="credit a unit for adding displayed liquidity, which is a payment to a maker")
    args = parser.parse_args()

    clips = [float(part) for part in args.clip.split(",") if part.strip()]
    paths = []
    for piece in args.input.split(","):
        paths.extend(sorted(glob.glob(piece)) or [piece])
    measured = []
    for path in paths:
        for symbol, rows in rows_of(path).items():
            stats = measure(rows, args.multiplier)
            if stats is None:
                continue
            stats["symbol"] = symbol
            for clip in clips:
                cost = (max(args.min_commission, args.fee_per_unit * clip) / clip
                        + args.extra_per_unit + args.regulatory_rate * stats["premium"]
                        - args.rebate_per_unit)
                margin = stats["half_usd"] - cost
                stats[f"rmax_{clip:g}"] = (stats["prints"] * margin / stats["sigma"]) if margin > 0 else None
            best = [(stats[f"rmax_{clip:g}"], clip) for clip in clips if stats[f"rmax_{clip:g}"] is not None]
            stats["best"] = max(best)[0] if best else None
            measured.append(stats)

    header = "| instrument | prints | median spread | half spread a unit | sigma a unit |"
    rule = "|---|---:|---:|---:|---:|"
    for clip in clips:
        header += f" R_max at {clip:g} |"
        rule += "---:|"
    print(f"### {args.label or args.input} -- {len(measured)} instruments, multiplier {args.multiplier}, "
          f"fee {args.fee_per_unit} a unit, floor {args.min_commission} an order, clips {args.clip}")
    print()
    print(header)
    print(rule)
    for stats in sorted(measured, key=lambda s: -(s["best"] if s["best"] is not None else -1e9)):
        line = (f"| {stats['symbol']} | {stats['prints']} | {stats['spread']:.4f} | {stats['half_usd']:.2f} | "
                f"{stats['sigma']:.2f} |")
        for clip in clips:
            value = stats[f"rmax_{clip:g}"]
            line += f" {value:.0f} |" if value is not None else " -- |"
        print(line)
    passing = [s for s in measured if s["best"] is not None and s["best"] >= REFERENCE]
    print()
    print(f"instruments whose best R_max clears the {REFERENCE:.0f} reference: "
          f"{len(passing)} of {len(measured)}")
    if passing:
        top = max(passing, key=lambda s: s["best"])
        print(f"highest: {top['symbol']} at {top['best']:.0f} prints a fill")
    return 0


if __name__ == "__main__":
    sys.exit(main())

