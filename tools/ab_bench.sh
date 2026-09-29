#!/usr/bin/env bash
#
# Paired A/B stage measurement for the L2 workload.
#
# Two builds are run in ABBA order within each block, so drift that is linear across a block cancels
# in the per-block difference. The per-block differences are averaged and reported with their t
# statistic, because a mean difference on its own cannot be told apart from measurement bias.
#
# That bias is real and it is not small. Running one binary as both A and B has produced +3% to +8% on
# every stage with |t| up to 3.3, with the sign differing between sessions, so a t of 2.8 is not by
# itself evidence. Pass "ctrl" as the fourth argument and the same session measures a null pair as
# well; the effect is only believed when it stands clear of the null in the same run.
#
# Every stage is printed, so the stages that were not supposed to move are visible next to the one that
# should. A change confined to one stage that moves another is a measurement problem, not a result.
#
# Usage:
#   tools/ab_bench.sh <binaryA> <binaryB> [blocks] [ctrl]
#
# With ctrl, each block is A B B A A A A A: the A-vs-A half is the null control for that block.
# Wall time is a total rather than a p50 and is much noisier, so its row will not track the stages.

set -u
cd "$(dirname "$0")/.." || exit 1

A="${1:-}"; B="${2:-}"; BLOCKS="${3:-5}"; MODE="${4:-}"
if [ -z "$A" ] || [ -z "$B" ]; then
  echo "usage: tools/ab_bench.sh <binaryA> <binaryB> [blocks] [ctrl]" >&2
  exit 2
fi

TRADES=data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
FULL_DEPTH=data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv
SLICE_2M=/tmp/depth_2m.csv
RAW=$(mktemp)
trap 'rm -f "$RAW"' EXIT

[ -f "$SLICE_2M" ] || head -n 2000001 "$FULL_DEPTH" > "$SLICE_2M"

# One profiled run. Stage p50 values are appended as "stage <label> <block> <stage> <p50>".
probe() {
  local bin=$1 label=$2 start end
  start=$(date +%s.%N)
  "$bin" l2_replay "$TRADES" "$SLICE_2M" BTC-PERPETUAL 0 active_l2 1 conservative \
    --no-output --fast-validation --profile-stages 2>&1 |
    awk -v label="$label" -v block="$BLOCK" '
      $1 == "[PROFILE]" && $3 ~ /^[0-9]+$/ { print "stage", label, block, $2, $6 }
    ' >> "$RAW"
  end=$(date +%s.%N)
  echo "wall $label $BLOCK $(echo "$end - $start" | bc)" >> "$RAW"
}

# The first run after the previous block ends is consistently slower, and ABBA assigns that position
# to A, which biases every result toward B. Measured: with 3 blocks, a null A-vs-A pair showed up to
# -13% with |t| = 3.6 when the pair straddled a block boundary, and within 0.6% when it did not. Burn
# one run per block instead of recording it.
warmup() {
  "$1" l2_replay "$TRADES" "$SLICE_2M" BTC-PERPETUAL 0 active_l2 1 conservative \
    --no-output --fast-validation > /dev/null 2>&1 || true
}

for BLOCK in $(seq 1 "$BLOCKS"); do
  warmup "$A"
  # ABBA cancels drift that is linear within the block, but it still assigns positions 1 and 4 to the
  # same label every block, so any position-dependent artifact lands on that label. Measured: with a
  # fixed order the stages that cannot be affected moved by -9% to -25% in the same direction as the
  # stage under test. Randomising which label gets the outer positions keeps the ABBA shape and makes
  # the position effect cancel in expectation across blocks.
  if [ $((RANDOM % 2)) -eq 0 ]; then
    probe "$A" A; probe "$B" B; probe "$B" B; probe "$A" A
  else
    probe "$B" B; probe "$A" A; probe "$A" A; probe "$B" B
  fi
  if [ "$MODE" = "ctrl" ]; then
    # The null pair: the same binary on both sides, measured in the same block as the effect.
    if [ $((RANDOM % 2)) -eq 0 ]; then
      probe "$A" A; probe "$A" C; probe "$A" C; probe "$A" A
    else
      probe "$A" C; probe "$A" A; probe "$A" A; probe "$A" C
    fi
  fi
  echo -n "."
done
echo " ${BLOCKS} blocks"

awk -v with_ctrl="$([ "$MODE" = "ctrl" ] && echo 1 || echo 0)" '
  /^stage/ { v[$2 SUBSEP $3 SUBSEP $4] = $5
    if (!($4 in seen_name)) { seen_name[$4] = 1; names[++nn] = $4 }
    if (!($3 in seen_block)) { seen_block[$3] = 1; blocks[++nb] = $3 }
  }
  /^wall/  { v["W" SUBSEP $2 SUBSEP $3] = $4 }

  function stats(   i, mean, ss, sd, se) {
    mean = 0
    for (i = 1; i <= nb; ++i) mean += diff[blocks[i]]
    mean /= nb
    if (nb < 2) { diff_mean = mean; tval = 0; return }
    ss = 0
    for (i = 1; i <= nb; ++i) ss += (diff[blocks[i]] - mean) ^ 2
    sd = sqrt(ss / (nb - 1))
    se = sd / sqrt(nb)
    diff_mean = mean
    tval = (se > 0) ? mean / se : 0
  }

  # One table row: the paired difference between two labels for the current `name`.
  function row(tag, la, lb, scale,   i, a, b, sa, sb) {
    sa = 0; sb = 0
    for (i = 1; i <= nb; ++i) {
      a = v[la SUBSEP blocks[i] SUBSEP name]
      b = v[lb SUBSEP blocks[i] SUBSEP name]
      diff[blocks[i]] = b - a
      sa += a; sb += b
    }
    stats()
    a_mean = sa / nb; b_mean = sb / nb
    pct = (a_mean > 0) ? diff_mean / a_mean * 100 : 0
    printf "%-14s %8s %9.*f %9.*f %8d %8.2f %+7.1f%%\n", name, tag, scale, a_mean, scale, b_mean, nb, tval, pct
  }

  END {
    printf "%-14s %8s %9s %9s %8s %8s %8s\n", "stage", "pair", "A mean", "B mean", "blocks", "t", "diff%"
    for (m = 1; m <= nn; ++m) {
      name = names[m]
      row("effect", "A", "B", 1)
      if (with_ctrl) row("null", "A", "C", 1)
    }
    # Wall time is a total rather than a percentile and is stored without a stage name, so it gets its
    # own loop instead of going through row().
    for (pass = 1; pass <= (with_ctrl ? 2 : 1); ++pass) {
      la = (pass == 1) ? "B" : "C"
      tag = (pass == 1) ? "effect" : "null"
      sa = 0; sb = 0
      for (i = 1; i <= nb; ++i) {
        a = v["W" SUBSEP "A" SUBSEP blocks[i]]
        b = v["W" SUBSEP la SUBSEP blocks[i]]
        diff[blocks[i]] = b - a
        sa += a; sb += b
      }
      stats()
      a_mean = sa / nb; b_mean = sb / nb
      pct = (a_mean > 0) ? diff_mean / a_mean * 100 : 0
      printf "%-14s %8s %9.3f %9.3f %8d %8.2f %+7.1f%%\n", "wall (s)", tag, a_mean, b_mean, nb, tval, pct
    }
  }
' "$RAW"
