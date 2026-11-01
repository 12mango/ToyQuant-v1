#!/usr/bin/env bash
#
# What the queue model decides on a US equity book, and where the fee floor comes from.
#
# Three questions that the recorded equities rows leave open, answered from files that are already on disk:
#
#   1. which queue model the tape supports. Every level decrease in an incremental depth stream is either a
#      trade or a cancellation, and only the trades are what the `conservative` model lets move a queue. The
#      attribution here is the one tools/calibrate_queue_model.py already makes for the perpetuals, run on the
#      same two-hour window, and it is the number that says whether `conservative` is a floor or a fiction.
#   2. whether the model changes the answer once the account's whole schedule is charged rather than the
#      commission alone. Four models, one window, one policy, the same schedule.
#   3. how much of the fee is the minimum per order, which is charged per order instance, so it depends on how
#      often the policy requotes rather than on how much it quotes. The clip sweep and the refresh sweep are
#      what separate those two.
#
# Every figure is a field of the engine's own summary, and the per-unit columns are derived from those fields
# rather than measured separately: a fee in ticks is fees_paid divided by the units filled and by the instrument's
# tick, and the floor's share is the same division applied to min_commission_paid.
#
# Usage:
#   tools/equity_schedule_sweep.sh [binary] [symbol] [day]
#
# Defaults to AAPL 2026-09-29, the window the account's schedule was first priced on. The files have to be
# present: they come from the paid Databento route, see tools/fetch_databento.py and data/README.md.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=${1:-out/build/linux-debug/toy_quant}
SYMBOL=${2:-AAPL}
DAY=${3:-2026-09-29}
TRADES=data/v2/equities/xnas_${SYMBOL}_trades_${DAY}_1330_1530.csv.gz
DEPTH=data/v2/equities/xnas_${SYMBOL}_depth_${DAY}_1330_1530.csv.gz
# The account's own terms, the same string the engine and tools/screen_spread.py take. The floor is the equity
# one: a 100 share order pays 0.35 of commission, so the floor binds only on the orders that fill less than that.
SCHEDULE="--fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013"
TICK=0.01

[ -x "$BIN" ] || { echo "binary not found or not executable: $BIN" >&2; exit 2; }
for file in "$TRADES" "$DEPTH"; do
  [ -f "$file" ] || { echo "missing $file: this window re-derives from a paid download" >&2; exit 2; }
done

. tools/lib.sh

# One run, reduced to the fields the table prints. The two derived columns are ticks a share filled, which is
# the unit the capture and the fee are both comparable in on a market whose tick is a cent whatever the price.
row() {
  local model=$1 size=$2 refresh=$3 output units fees floor capture markout closed net
  # "default" is the condition every recorded equities row was measured under, so the first table keeps it: a
  # run that names no refresh is the only one comparable with the F4 table, and the sweep below exists to say
  # what naming one changes.
  local refresh_flag=""
  [ "$refresh" = "default" ] || refresh_flag="--refresh-price-ticks=$refresh"
  # shellcheck disable=SC2086
  output=$("$BIN" l2_replay "$TRADES" "$DEPTH" "$SYMBOL" 0 active_l2 1 "$model" \
    --order-size="$size" --inventory-limit=$((size * 3)) --base-spread-ticks=1 \
    $refresh_flag $SCHEDULE 2>&1)
  units=$(field_of "$output" trade_report_quantity)
  fees=$(field_of "$output" fees_paid)
  floor=$(field_of "$output" min_commission_paid)
  capture=$(field_of "$output" captured_edge_per_unit_ticks)
  markout=$(field_of "$output" markout_per_unit_ticks)
  closed=$(field_of "$output" closed_pnl_usd)
  net=$(field_of "$output" net_pnl_usd)
  awk -v m="$model" -v s="$size" -v r="$refresh" -v u="$units" -v f="$fees" -v fl="$floor" \
      -v c="$capture" -v k="$markout" -v cl="$closed" -v n="$net" -v t="$TICK" \
      -v o="$(field_of "$output" submitted_orders)" \
      -v fills="$(field_of "$output" trade_reports)" 'BEGIN {
        if (u + 0 == 0) { printf "| %s | %s | %s | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |\n", m, s, r; exit }
        printf "| %s | %s | %s | %s | %s | %s | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f |\n",
          m, s, r, o, fills, u, c, k, f / u / t, fl / u / t, cl, n
      }'
}

# One run under a named schedule, for the table that says what the fee arrangements are worth. The column that
# matters is gross_closed_pnl_usd, which the engine reports with the fees added back: it is the same number under
# every schedule here, because the fills are the same, and it is the number no fee tier can move.
schedule_row() {
  local name=$1 schedule=$2 output units fees floor capture markout gross net
  # shellcheck disable=SC2086
  output=$("$BIN" l2_replay "$TRADES" "$DEPTH" "$SYMBOL" 0 active_l2 1 prorata --order-size=100 \
    --inventory-limit=300 --base-spread-ticks=1 $schedule 2>&1)
  units=$(field_of "$output" trade_report_quantity)
  fees=$(field_of "$output" fees_paid)
  floor=$(field_of "$output" min_commission_paid)
  capture=$(field_of "$output" captured_edge_per_unit_ticks)
  markout=$(field_of "$output" markout_per_unit_ticks)
  gross=$(field_of "$output" gross_closed_pnl_usd)
  net=$(field_of "$output" net_pnl_usd)
  awk -v m="$name" -v u="$units" -v f="$fees" -v fl="$floor" -v c="$capture" -v k="$markout" \
      -v g="$gross" -v n="$net" -v t="$TICK" -v o="$(field_of "$output" submitted_orders)" \
      -v fills="$(field_of "$output" trade_reports)" 'BEGIN {
        if (u + 0 == 0) { printf "| %s | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |\n", m; exit }
        printf "| %s | %s | %s | %s | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f |\n",
          m, o, fills, u, c, k, f / u / t, fl / u / t, g, n
      }'
}

echo "## Queue-model attribution, $SYMBOL $DAY"
echo
python3 tools/calibrate_queue_model.py "$DEPTH" "$TRADES" --max-rows=400000 | head -12
echo
echo "## Four models, the same window and policy, the account's whole schedule"
echo
HEADER="| model | clip | refresh | order instances | fills | units | capture ticks/unit | markout ticks/unit | fee ticks/unit | of which the floor | closed USD | net USD |"
RULE="|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
echo "$HEADER"
echo "$RULE"
row conservative 100 default
row prorata 100 default
row lumpy 100 default
row optimistic 100 default
echo
echo "## The floor against the clip, prorata at the default refresh"
echo
echo "$HEADER"
echo "$RULE"
row prorata 100 default
row prorata 500 default
row prorata 1000 default
row prorata 2000 default
echo
echo "## The refresh, which is what sets the number of order instances"
echo
echo "$HEADER"
echo "$RULE"
row prorata 100 1
row prorata 100 4
row prorata 100 16
row prorata 100 64
row prorata 1000 1
row prorata 1000 4
row prorata 1000 16
row prorata 1000 64
echo
echo "## What the account's schedule can change: four arrangements, same fills"
echo
echo "| schedule | order instances | fills | units | capture ticks/unit | markout ticks/unit | fee ticks/unit | of which the floor | gross closed USD | net USD |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
schedule_row "commission only" ""
schedule_row "tiered, no rebate" "--fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0"
schedule_row "tiered, rebate" "$SCHEDULE"
schedule_row "tiered, rebate, no floor" "--fee-schedule=min=0,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013"
