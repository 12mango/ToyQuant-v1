#!/usr/bin/env bash
#
# The non-market-making signal, measured over a grid instead of argued about. Each row is one replay of the
# same 200k-row slice with a different --trend-params and the same sizing, and the CSV carries what the run
# itself reported: fills, net PnL, fees, risk pauses and the peak position.
#
# This is a measurement, not a claim: the first grid was negative in every cell, which is the useful result.
# A taker strategy pays the spread and the taker fee on every clip, and the EWMA cross as written has no
# forecast that covers them. The sweep is how a change is judged before it is believed.
#
# Usage:
#   tools/trend_sweep.sh [binary] [trades] [depth] [symbol] [output_csv]
#
# Override the grid with environment variables:
#   FAST_SLOW="5:30 10:60 20:120"   fast view first, slow view second, separated by ':'
#   ENTRIES="1 2 3"                 entry band in ticks
#   EXITS="0.5"                     exit band in ticks
#   STOPS="4 6 8"                   stop distance in ticks
#   COOLDOWN=60 HOLDING=2000        views after a stop, and the time stop
#   ORDER_SIZE=10 INVENTORY=100     the clip and the full-signal target
#
# The default grid is 3 x 3 x 3 = 27 runs. Set FAST_SLOW/ENTRIES/STOPS to a single value for a quick check.

set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${1:-out/build/linux-debug/toy_quant}"
TRADES="${2:-data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz}"
DEPTH="${3:-/tmp/depth_200k_base.csv}"
SYMBOL="${4:-BTC-PERPETUAL}"
OUT="${5:-/tmp/trend_sweep.csv}"

FAST_SLOW="${FAST_SLOW:-5:30 10:60 20:120}"
ENTRIES="${ENTRIES:-1 2 3}"
EXITS="${EXITS:-0.5}"
STOPS="${STOPS:-4 6 8}"
COOLDOWN="${COOLDOWN:-60}"
HOLDING="${HOLDING:-2000}"
ORDER_SIZE="${ORDER_SIZE:-10}"
INVENTORY="${INVENTORY:-100}"

if [ ! -x "$BIN" ]; then
  echo "trend: $BIN is not executable; build the linux-debug preset first" >&2
  exit 1
fi
if [ ! -f "$DEPTH" ]; then
  echo "trend: $DEPTH is missing; run tools/verify_l2.sh fast once to cut it" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
printf 'fast,slow,entry_ticks,exit_ticks,stop_ticks,fills,net_pnl_usd,fees_paid,risk_pauses,max_abs_inventory,recon\n' > "$OUT"
ROWS=0

field() {
  printf '%s\n' "$1" | grep -o " $2=[^ ]*" | head -1 | cut -d= -f2
}

for pair in $FAST_SLOW; do
  fast=${pair%%:*}
  slow=${pair##*:}
  for entry in $ENTRIES; do
    for exit_band in $EXITS; do
      for stop in $STOPS; do
        params="$fast,$slow,$entry,$exit_band,$stop,$COOLDOWN,$HOLDING"
        "$BIN" l2_replay "$TRADES" "$DEPTH" "$SYMBOL" 0 trend_l2 1 conservative \
          --fast-validation --paper-broker --order-size="$ORDER_SIZE" --inventory-limit="$INVENTORY" \
          --trend-params="$params" > "$TMP/run.log" 2>&1
        rc=$?
        if [ $rc -ne 0 ]; then
          echo "trend: $params exited $rc" >&2
          tail -5 "$TMP/run.log" | sed 's/^/  /' >&2
          exit 1
        fi

        fills=$(field "$(cat "$TMP/run.log")" trade_reports)
        net=$(field "$(cat "$TMP/run.log")" net_pnl_usd)
        fees=$(field "$(cat "$TMP/run.log")" fees_paid)
        pauses=$(field "$(cat "$TMP/run.log")" risk_pause_count)
        peak=$(field "$(cat "$TMP/run.log")" max_abs_inventory)
        recon=bad
        grep -q '\[RECON\].*status=ok' "$TMP/run.log" && recon=ok
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
          "$fast" "$slow" "$entry" "$exit_band" "$stop" "${fills:-?}" "${net:-?}" \
          "${fees:-?}" "${pauses:-?}" "${peak:-?}" "$recon" >> "$OUT"
        ROWS=$((ROWS + 1))
        if [ "$recon" != "ok" ]; then
          echo "trend: $params did not reconcile" >&2
          exit 1
        fi
      done
    done
  done
done

echo "trend sweep: $OUT ($ROWS rows, order_size=$ORDER_SIZE inventory=$INVENTORY)"
echo "best by net_pnl_usd:"
{ head -1 "$OUT"; tail -n +2 "$OUT" | sort -t, -k7,7gr | head -5; } | column -t -s,
