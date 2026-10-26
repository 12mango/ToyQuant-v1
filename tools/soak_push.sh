#!/usr/bin/env bash
#
# A live push-chain soak: the two paths a six-second run cannot reach, a fixed-cadence heartbeat and a
# forced reconnect, and the check that makes the run worth keeping -- the engine's own recording replays
# to the same summary. It fails on a sequence gap, a parse error, a ring wait, a reconciliation break, or
# a field that moved between the live run and its replay.
#
# Usage:
#   tools/soak_push.sh                     # 30 minutes, a ping every 20 s, a reconnect every 25 min
#   DURATION=7200 DROP=1500 tools/soak_push.sh
#
# Environment: DURATION (seconds), PING (seconds, 0 = idle-only), DROP (seconds, 0 = never), SYMBOL,
# STRATEGY, PORT, OUT (prefix for logs and recordings), BIN.

set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${BIN:-out/build/linux-debug/toy_quant}"
DURATION="${DURATION:-1800}"
PING="${PING:-20}"
DROP="${DROP:-1500}"
SYMBOL="${SYMBOL:-BTC-USDT-SWAP}"
STRATEGY="${STRATEGY:-active_l2}"
PORT="${PORT:-8765}"
OUT="${OUT:-/tmp/toyquant_soak}"

[ -x "$BIN" ] || { echo "binary not found or not executable: $BIN" >&2; exit 2; }
[ -f tools/venue_relay.py ] || { echo "tools/venue_relay.py is missing" >&2; exit 2; }

rm -f "${OUT}.jsonl" "${OUT}_relay_trades.csv" "${OUT}_relay_depth.csv" \
      "${OUT}_eng_trades.csv" "${OUT}_eng_depth.csv" \
      "${OUT}_relay.log" "${OUT}_eng.log" "${OUT}_replay.log"

python3 tools/venue_relay.py --instrument "$SYMBOL" --listen "127.0.0.1:${PORT}" \
  --capture "${OUT}.jsonl" --record "${OUT}_relay" --max-seconds "$DURATION" \
  --ping-seconds "$PING" --drop-after-seconds "$DROP" > "${OUT}_relay.log" 2>&1 &
relay=$!
sleep 1
"$BIN" ws "127.0.0.1:${PORT}" "$SYMBOL" "$STRATEGY" 1 prorata --fast-validation \
  --paper-broker --ws-seconds="$DURATION" --record="${OUT}_eng" > "${OUT}_eng.log" 2>&1
engine=$?

# The relay's own deadline starts when the engine connects, so it may end a moment later; wait for it
# to write its summary rather than killing it the instant the engine returns.
for _ in $(seq 1 30); do kill -0 "$relay" 2>/dev/null || break; sleep 1; done
kill "$relay" 2>/dev/null
wait "$relay" 2>/dev/null

"$BIN" l2_replay "${OUT}_eng_trades.csv" "${OUT}_eng_depth.csv" "$SYMBOL" 0 "$STRATEGY" 1 prorata \
  --fast-validation --paper-broker > "${OUT}_replay.log" 2>&1
replay=$?

field() {
  grep -oE "(^| )$1=[-0-9.]+" "$2" | head -1 | sed 's/^ //' | cut -d= -f2
}

fail=0
for name in submitted_orders trade_reports realized_pnl fees_paid max_abs_inventory; do
  live=$(field "$name" "${OUT}_eng.log")
  back=$(field "$name" "${OUT}_replay.log")
  if [ "$live" != "$back" ]; then
    echo "FAIL $name live=$live replay=$back" >&2
    fail=1
  fi
done
grep -q ' gaps=0' "${OUT}_relay.log" || { echo "FAIL relay reported a sequence gap" >&2; fail=1; }
grep -q 'parse_errors=0' "${OUT}_eng.log" || { echo "FAIL the feed reported a parse error" >&2; fail=1; }
grep -q 'ring_waits=0' "${OUT}_eng.log" || { echo "FAIL the feed waited on a full ring" >&2; fail=1; }
grep -q 'status=ok' "${OUT}_eng.log" || { echo "FAIL reconciliation broke" >&2; fail=1; }
if [ "$DROP" -gt 0 ] 2>/dev/null; then
  expected=$((DURATION / DROP))
  reconnects=$(field reconnects "${OUT}_relay.log")
  if [ "$reconnects" != "$expected" ]; then
    echo "FAIL reconnects=$reconnects, expected $expected" >&2
    fail=1
  fi
fi

if [ "$fail" -eq 0 ]; then
  echo "PASS ${DURATION}s $(grep -oE 'trades=[0-9]+ book_messages=[0-9]+ depth_rows=[0-9]+' "${OUT}_relay.log" | tail -1)"
  echo "     logs: ${OUT}_relay.log ${OUT}_eng.log ${OUT}_replay.log"
else
  echo "FAIL logs: ${OUT}_relay.log ${OUT}_eng.log ${OUT}_replay.log" >&2
fi
exit "$fail"
