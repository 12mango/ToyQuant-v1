#!/usr/bin/env bash
#
# What the pre-trade controls do, measured on the 200k-row L2 slice rather than argued about. Four runs:
#
#   baseline    no risk flags. It must print no [RISK] line, and its [EXECUTION] line is the reference.
#   permissive  a wide band and a high rate: both controls are on, neither should refuse anything, and
#               every execution counter must equal the baseline's. This is the check that a control which
#               is merely enabled does not change the run.
#   fat         1 basis point, which refuses a quote at the touch of a one-tick spread and shows the counter.
#   rate        one order per hundred seconds, which shows the bucket refusing the strategy's retries.
#
# Every run must still reconcile. The script exits nonzero on the first invariant that does not hold, so
# tier 1 of tools/verify_l2.sh can call it; it is also useful on its own after a change to the risk path.
#
# Usage: tools/risk_check.sh [binary] [trades] [depth] [symbol]

set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${1:-out/build/linux-debug/toy_quant}"
TRADES="${2:-data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz}"
DEPTH="${3:-/tmp/depth_200k_base.csv}"
SYMBOL="${4:-BTC-PERPETUAL}"

if [ ! -x "$BIN" ]; then
  echo "risk: $BIN is not executable; build the linux-debug preset first" >&2
  exit 1
fi
if [ ! -f "$DEPTH" ]; then
  echo "risk: $DEPTH is missing; run tools/verify_l2.sh fast once to cut it" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
STATUS=0

run() {
  local name=$1
  shift
  "$BIN" l2_replay "$TRADES" "$DEPTH" "$SYMBOL" 0 trend_l2 1 conservative \
    --fast-validation --paper-broker "$@" > "$TMP/$name.log" 2>&1
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "risk: $name exited $rc" >&2
    tail -8 "$TMP/$name.log" | sed 's/^/  /' >&2
    STATUS=1
  fi
}

submitted() {
  grep -o 'submitted_orders=[0-9]*' "$1" | head -1 | cut -d= -f2
}

risk_field() {
  grep -o " $2=[0-9]*" "$1" | head -1 | cut -d= -f2
}

reconciles() {
  grep -q '\[RECON\].*status=ok' "$1"
}

run baseline
run permissive --fat-finger-bps=100 --max-orders-per-second=1000 --order-burst=1000
run fat --fat-finger-bps=1
run rate --max-orders-per-second=0.01 --order-burst=1

BASE_SUBMITTED=$(submitted "$TMP/baseline.log")
PERMISSIVE_SUBMITTED=$(submitted "$TMP/permissive.log")
FAT_SUBMITTED=$(submitted "$TMP/fat.log")
RATE_SUBMITTED=$(submitted "$TMP/rate.log")
FAT_REJECTIONS=$(risk_field "$TMP/fat.log" fat_finger_rejections)
RATE_REJECTIONS=$(risk_field "$TMP/rate.log" rate_limited_orders)

BASE_EXEC=$(grep '^\[EXECUTION\]' "$TMP/baseline.log" | head -1)
PERMISSIVE_EXEC=$(grep '^\[EXECUTION\]' "$TMP/permissive.log" | head -1)

echo "risk baseline: submitted=${BASE_SUBMITTED:-?} risk_line=$(grep -c '^\[RISK\]' "$TMP/baseline.log")" \
     "recon=$(reconciles "$TMP/baseline.log" && echo ok || echo bad)"
echo "risk permissive: submitted=${PERMISSIVE_SUBMITTED:-?} execution_same=$([ "$BASE_EXEC" = "$PERMISSIVE_EXEC" ] && echo yes || echo no)" \
     "recon=$(reconciles "$TMP/permissive.log" && echo ok || echo bad)"
echo "risk fat: submitted=${FAT_SUBMITTED:-?} fat_finger_rejections=${FAT_REJECTIONS:-?}" \
     "recon=$(reconciles "$TMP/fat.log" && echo ok || echo bad)"
echo "risk rate: submitted=${RATE_SUBMITTED:-?} rate_limited_orders=${RATE_REJECTIONS:-?}" \
     "recon=$(reconciles "$TMP/rate.log" && echo ok || echo bad)"

for name in baseline permissive fat rate; do
  if ! reconciles "$TMP/$name.log"; then
    echo "risk: $name did not reconcile" >&2
    STATUS=1
  fi
done
if grep -q '^\[RISK\]' "$TMP/baseline.log"; then
  echo "risk: the no-flag run printed a [RISK] line" >&2
  STATUS=1
fi
if [ -z "$BASE_SUBMITTED" ] || [ -z "$PERMISSIVE_SUBMITTED" ]; then
  echo "risk: a run did not print submitted_orders" >&2
  STATUS=1
fi
if [ "$BASE_EXEC" != "$PERMISSIVE_EXEC" ]; then
  echo "risk: enabling permissive controls changed the [EXECUTION] line" >&2
  STATUS=1
fi
if ! grep -q '^\[RISK\]' "$TMP/fat.log" || [ "${FAT_REJECTIONS:-0}" = "0" ]; then
  echo "risk: the 1 bp band did not refuse anything" >&2
  STATUS=1
fi
if ! grep -q '^\[RISK\]' "$TMP/rate.log" || [ "${RATE_REJECTIONS:-0}" = "0" ]; then
  echo "risk: the rate bucket did not refuse anything" >&2
  STATUS=1
fi
if [ -n "$FAT_SUBMITTED" ] && [ -n "$BASE_SUBMITTED" ] && [ "$FAT_SUBMITTED" -gt "$BASE_SUBMITTED" ]; then
  echo "risk: the fat-finger run submitted more orders than the baseline" >&2
  STATUS=1
fi
if [ -n "$RATE_SUBMITTED" ] && [ -n "$BASE_SUBMITTED" ] && [ "$RATE_SUBMITTED" -gt "$BASE_SUBMITTED" ]; then
  echo "risk: the rate-limited run submitted more orders than the baseline" >&2
  STATUS=1
fi

if [ $STATUS -eq 0 ]; then
  echo "risk OK"
fi
exit $STATUS
