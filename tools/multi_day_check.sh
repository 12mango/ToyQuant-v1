#!/usr/bin/env bash
#
# One measured row per Deribit day, in the format the out-of-sample check uses.
#
# The out-of-sample section of docs/PERFORMANCE_HISTORY.md reports the same policy on a second day through the
# same depth format, which is what makes a difference between rows a difference of day rather than of feed. Its
# row is cut from the day's 25-level snapshot, and this script generalises that: for every day present under
# data/v2/ it cuts the same slice, runs the same command, and prints the same fields, so adding a day costs one
# downloaded file pair and one command.
#
# It also prints the check block for each day. The rows in the document are re-derived by tools/check_docs.py on
# every gate run, and a hand-copied row is exactly how a number drifts.
#
# Place the two free Tardis samples for a day under data/v2/, both named after the day and the instrument:
#   data/v2/deribit_book_snapshot_25_YYYY-MM-DD_BTC-PERPETUAL.csv.gz
#   data/v2/deribit_trades_YYYY-MM-DD_BTC-PERPETUAL.csv.gz
#
# Usage:
#   tools/multi_day_check.sh                  # every day found under data/v2/
#   tools/multi_day_check.sh 2020-06-01 ...   # only the days named on the command line
#
# Binary defaults to out/build/linux-debug/toy_quant, as in tools/verify_l2.sh. Exit status is 0 when every
# requested day produced a row, and 1 when a day was missing, unreadable, or produced no summary.

set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${BIN:-out/build/linux-debug/toy_quant}"
SLICE_ROWS=200000

[ -x "$BIN" ] || { echo "binary not found or not executable: $BIN" >&2; exit 2; }

day_of() {
  printf '%s' "$1" | sed -n 's/.*_\([0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\)_BTC-PERPETUAL\.csv\(\.gz\)\{0,1\}$/\1/p'
}

# First occurrence of field=value in the run's whole output, C++ default formatting included. This mirrors how
# tools/check_docs.py reads a recorded block, so a row this script prints is a row that tool can verify.
field_of() {
  printf '%s' "$1" | grep -oE "(^|[^A-Za-z0-9_])$2=[^ ]+" | head -n 1 | sed 's/^[^A-Za-z0-9_]//; s/^[A-Za-z_][A-Za-z0-9_]*=//'
}

# Ratio of two decimal fields as the "times the captured edge" the table quotes, or n/a when the edge is zero.
ratio_of() {
  awk -v fee="$1" -v edge="$2" 'BEGIN { if (edge + 0 == 0) print "n/a"; else printf "%.1fx", fee / edge }'
}

if [ "$#" -gt 0 ]; then
  days=("$@")
else
  days=()
  for f in data/v2/deribit_book_snapshot_25_*_BTC-PERPETUAL.csv data/v2/deribit_book_snapshot_25_*_BTC-PERPETUAL.csv.gz; do
    [ -e "$f" ] || continue
    day=$(day_of "$f")
    [ -n "$day" ] && days+=("$day")
  done
  # A day stored in both forms would otherwise be listed twice.
  if [ "${#days[@]}" -gt 0 ]; then
    readarray -t days < <(printf '%s\n' "${days[@]}" | sort -u)
  fi
fi

if [ "${#days[@]}" -eq 0 ]; then
  echo "no day found: expected data/v2/deribit_book_snapshot_25_YYYY-MM-DD_BTC-PERPETUAL.csv.gz"
  exit 1
fi

printf '%-12s %-9s %8s %6s %8s %12s %12s %9s %9s %11s\n' \
  day feed orders fills fill_rate edge_ticks edge_usd fee fee_per_edge queue_p50

rc=0
for day in "${days[@]}"; do
  snapshot="data/v2/deribit_book_snapshot_25_${day}_BTC-PERPETUAL.csv.gz"
  # The control day is stored uncompressed in this repository, so a day is any pair of files named after it.
  [ -f "$snapshot" ] || snapshot="data/v2/deribit_book_snapshot_25_${day}_BTC-PERPETUAL.csv"
  trades="data/v2/deribit_trades_${day}_BTC-PERPETUAL.csv.gz"
  slice="/tmp/snap_$(printf '%s' "$day" | tr -d -)_200k.csv"

  missing=0
  for file in "$snapshot" "$trades"; do
    [ -f "$file" ] || { echo "$day missing $file"; missing=1; }
  done
  [ "$missing" -eq 0 ] || { rc=1; continue; }

  # A truncated download is silent otherwise: every reader would still parse a prefix of the day.
  for file in "$snapshot" "$trades"; do
    case "$file" in
      *.gz) gzip -t "$file" 2>/dev/null || { echo "$day $file is not a complete gzip"; missing=1; } ;;
    esac
  done
  [ "$missing" -eq 0 ] || { rc=1; continue; }

  if [ ! -f "$slice" ]; then
    case "$snapshot" in
      *.gz) zcat "$snapshot" | head -n $((SLICE_ROWS + 1)) > "$slice" ;;
      *) head -n $((SLICE_ROWS + 1)) "$snapshot" > "$slice" ;;
    esac
  fi
  rows=$(wc -l < "$slice")
  [ "$rows" -gt "$SLICE_ROWS" ] || echo "$day warning: slice has $rows rows, fewer than the $SLICE_ROWS the other days use"

  output=$("$BIN" l2_replay "$trades" "$slice" BTC-PERPETUAL 0 active_l2 1 prorata \
    --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 2>&1)
  if ! printf '%s' "$output" | grep -q 'submitted_orders='; then
    echo "$day no summary in the run output; it ended with:"
    printf '%s\n' "$output" | tail -3 | sed 's/^/  /'
    rc=1
    continue
  fi

  orders=$(field_of "$output" submitted_orders)
  fills=$(field_of "$output" trade_reports)
  fill_rate=$(field_of "$output" fill_rate)
  edge_ticks=$(field_of "$output" captured_edge_per_unit_ticks)
  edge_usd=$(field_of "$output" captured_edge_usd)
  fee=$(field_of "$output" fees_paid)
  pnl=$(field_of "$output" realized_pnl)
  queue_p50=$(field_of "$output" queue_min_fraction_p50)
  line=" submitted_orders=$orders trade_reports=$fills fill_rate=$fill_rate"
  line="$line captured_edge_per_unit_ticks=$edge_ticks captured_edge_usd=$edge_usd fees_paid=$fee"
  line="$line realized_pnl=$pnl queue_min_fraction_p50=$queue_p50"

  printf '%-12s %-9s %8s %6s %8s %12s %12s %9s %9s %11s\n' \
    "$day" "snapshot" "$orders" "$fills" "$fill_rate" "$edge_ticks" "$edge_usd" "$fee" \
    "$(ratio_of "$fee" "$edge_usd")" "$queue_p50"

  printf '\n%s\n' '<!-- toyquant:check'
  printf 'run: l2_replay %s %s BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4\n' \
    "$trades" "$slice"
  printf 'then:%s\n' "$line"
  printf '%s\n\n' '-->'
done

exit $rc
