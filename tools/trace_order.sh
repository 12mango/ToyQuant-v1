#!/usr/bin/env bash
#
# Trace the order that waited longest, in one command.
#
# `--trace-order=N` needs an order id and a reader does not have one before the run. A traced run therefore
# also prints [TRACE_HINTS], which is enough in two runs but not for the first question a reader asks. This
# script does both: a cheap run to collect the hints, then the run that traces the order they name.
#
# Usage:
#   tools/trace_order.sh [extra l2_replay flags...]
#
# Environment:
#   WHICH=filled|working   which hint to follow, default filled. A filled order has a fill to explain; a
#                          working order is the one that is still waiting when the input ends.
#   BIN=...                binary to run, default out/build/linux-debug/toy_quant.
#   DEPTH=...              depth slice, default /tmp/depth_200k_base.csv.
#
# The window is the one the design review quotes and the one tools/verify_l2.sh builds its slices for.

set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${BIN:-out/build/linux-debug/toy_quant}"
TRADES=data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
DEPTH="${DEPTH:-/tmp/depth_200k_base.csv}"
WHICH="${WHICH:-filled}"

if [ ! -f "$DEPTH" ]; then
  echo "missing depth slice: $DEPTH" >&2
  echo "tools/verify_l2.sh fast builds it from the tracked full depth file" >&2
  exit 2
fi

# --trace-order=1 is only here to make the hints print; the id it traces is dropped, because the point of
# this script is to find a better one.
WINDOW=(
  l2_replay "$TRADES" "$DEPTH" BTC-PERPETUAL 0 active_l2 1 prorata
  --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
)

hints=$("$BIN" "${WINDOW[@]}" --trace-order=1 "$@" 2>&1 | grep -m1 TRACE_HINTS)
if [ -z "$hints" ]; then
  echo "no [TRACE_HINTS] in the run; check the binary and the input files" >&2
  exit 1
fi

if [ "$WHICH" = "working" ]; then
  id=$(printf '%s' "$hints" | sed 's/.*longest_wait_working_order_id=\([0-9]*\).*/\1/')
else
  id=$(printf '%s' "$hints" | sed 's/.*longest_wait_filled_order_id=\([0-9]*\).*/\1/')
fi

echo "$hints"
if [ -z "$id" ] || [ "$id" = "0" ]; then
  echo "no $WHICH order to trace in this window; try WHICH=working or a longer window" >&2
  exit 1
fi

echo "--- order $id, the longest $WHICH wait in this window ---"
"$BIN" "${WINDOW[@]}" --trace-order="$id" "$@" 2>&1 | grep QUEUE_TRACE
