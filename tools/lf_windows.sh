#!/usr/bin/env bash
#
# One measured row per equity window: the pull chain, run as a batch.
#
# The L2 replay path is already the pull chain -- a file pair in, the same book, strategy, engine and portfolio
# out -- but the recorded runs take one window at a time. A pull chain is many windows, so this loops over every
# equity pair present under data/v2/equities/, runs the same slow configuration on each, and prints the fields a
# mid/low-frequency decision is judged by: fills, fees, net PnL and inventory, rather than lateness.
#
# The depth files here are a snapshot per row, so the queue model is not exercised; that is the shape of the
# converted Databento data, not a choice. The queue lives on the push chain, not this one.
#
# Usage:
#   tools/lf_windows.sh                 # every symbol with a complete pair
#   tools/lf_windows.sh MU SPY ...      # only the symbols named
#
# The slow configuration is the point, and it is overridable through the environment:
#   STRATEGY, QUEUE_MODEL, ORDER_SIZE, INVENTORY_LIMIT, BASE_SPREAD_TICKS, REFRESH_TICKS
#
# Binary defaults to out/build/linux-debug/toy_quant, as in tools/verify_l2.sh. Exit status is 0 when every
# window produced a row, 1 when a pair was missing or a run produced no summary.

set -u
cd "$(dirname "$0")/.." || exit 1
. tools/lib.sh

BIN="${BIN:-out/build/linux-debug/toy_quant}"
STRATEGY="${STRATEGY:-inventory_aware_l2}"
QUEUE_MODEL="${QUEUE_MODEL:-conservative}"
ORDER_SIZE="${ORDER_SIZE:-100}"
INVENTORY_LIMIT="${INVENTORY_LIMIT:-500}"
BASE_SPREAD_TICKS="${BASE_SPREAD_TICKS:-2}"
REFRESH_TICKS="${REFRESH_TICKS:-50}"

[ -x "$BIN" ] || { echo "binary not found or not executable: $BIN" >&2; exit 2; }

symbol_of() {
  printf '%s' "$1" | sed -n 's#.*/xnas_\([A-Za-z0-9.]*\)_trades_.*#\1#p'
}

day_of() {
  printf '%s' "$1" | sed -n 's#.*_trades_\([0-9-]*\)_1330_1530\.csv\(\.gz\)\{0,1\}$#\1#p'
}

if [ "$#" -gt 0 ]; then
  symbols=("$@")
else
  symbols=()
  for f in data/v2/equities/xnas_*_trades_*_1330_1530.csv.gz; do
    [ -e "$f" ] || continue
    symbols+=("$(symbol_of "$f")")
  done
  readarray -t symbols < <(printf '%s\n' "${symbols[@]}" | sort -u)
fi

if [ "${#symbols[@]}" -eq 0 ] || [ -z "${symbols[0]:-}" ]; then
  echo "no equity pair found: expected data/v2/equities/xnas_<SYMBOL>_trades_<day>_1330_1530.csv.gz"
  exit 1
fi

printf '%-7s %-12s %8s %10s %12s %10s %10s %10s\n' \
  symbol day fills fees net_pnl max_inv quotes cancels

rc=0
for symbol in "${symbols[@]}"; do
  for trades in data/v2/equities/xnas_"${symbol}"_trades_*_1330_1530.csv.gz; do
    [ -e "$trades" ] || continue
    day=$(day_of "$trades")
    [ -n "$day" ] || continue
    depth=$(printf '%s' "$trades" | sed 's#_trades_#_depth_#')
    if [ ! -f "$depth" ]; then
      echo "$symbol $day missing $depth" >&2
      rc=1
      continue
    fi

    output=$("$BIN" l2_replay "$trades" "$depth" "$symbol" 0 "$STRATEGY" 1 "$QUEUE_MODEL" \
      --fast-validation --order-size="$ORDER_SIZE" --inventory-limit="$INVENTORY_LIMIT" \
      --base-spread-ticks="$BASE_SPREAD_TICKS" --refresh-price-ticks="$REFRESH_TICKS" 2>&1)
    if ! printf '%s' "$output" | grep -q 'trade_reports='; then
      echo "$symbol $day no summary in the run output; it ended with:" >&2
      printf '%s\n' "$output" | tail -3 | sed 's/^/  /' >&2
      rc=1
      continue
    fi

    fills=$(field_of "$output" trade_reports)
    fees=$(field_of "$output" fees_paid)
    pnl=$(field_of "$output" net_pnl_usd)
    max_inv=$(field_of "$output" max_abs_inventory)
    quotes=$(field_of "$output" quote_count)
    cancels=$(field_of "$output" cancel_count)
    printf '%-7s %-12s %8s %10s %12s %10s %10s %10s\n' \
      "$symbol" "$day" "$fills" "$fees" "$pnl" "$max_inv" "$quotes" "$cancels"
  done
done

exit $rc
