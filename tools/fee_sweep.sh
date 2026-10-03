#!/usr/bin/env bash
#
# Sweep the maker fee rate over the windows the documents quote, and print what each combination earned.
#
# The fee is the one term of a market maker's PnL that is a schedule rather than a result, so the question
# "would a different fee have made this profitable" has an exact answer per window and is worth a table
# rather than an argument. Each row reports the run's total PnL before fees, the same split into what closed
# and what the run left open, the notional the maker fees were charged on, and the maker rate that would have
# made the window exactly flat. A negative breakeven rate is the answer the fee question is asking about: no
# fee at all would still lose money, and that size of rebate would be needed to break even.
#
# The default rates are the Deribit maker fee the instrument carries, zero, and a rebate half the size of the
# published fee. The windows are the two mainline slices, the second day's snapshot slice and the two million
# record slice, all of which tools/verify_l2.sh cuts on demand.
#
# Usage:
#   tools/fee_sweep.sh [binary]
#
# Prints a markdown table, then the recorded block that tools/check_docs.py re-derives, so pasting both is
# the whole job of recording the result.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=${1:-out/build/linux-debug/toy_quant}
RATES="0.0002 0 -0.0002"
STRATEGY_ARGS=(--fast-validation --base-spread-ticks=1 --refresh-price-ticks=4)
# name|trades|depth|rates, where the last field overrides the three above for a window whose sweep is already
# settled at one rate. The two million record window is the largest sample and costs about three seconds a run,
# so it is quoted at the venue's own rate rather than at all three.
WINDOWS=(
  "0401_incremental_200k|data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz|/tmp/depth_200k_base.csv|"
  "0401_snapshot_200k|data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz|/tmp/snap_20200401_200k.csv|"
  "0501_snapshot_200k|data/v2/crypto/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz|/tmp/snap_0501_200k.csv|"
  "0401_incremental_2m|data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz|/tmp/depth_2m.csv|0.0002"
)

# First occurrence of field=value in a run's output, which is how tools/check_docs.py reads a recorded block.
field_of() {
  printf '%s' "$1" | grep -oE "(^| )$2=[^ ]+" | head -n 1 | sed 's/^ //; s/^[A-Za-z_][A-Za-z0-9_]*=//'
}

echo "| window | maker rate | fills | net USD | gross USD | closed USD | gross/fill USD | breakeven rate |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|"
table=""
block=""
for window in "${WINDOWS[@]}"; do
  IFS='|' read -r name trades depth window_rates <<<"$window"
  [ -n "$window_rates" ] || window_rates=$RATES
  for rate in $window_rates; do
    output=$("$BIN" l2_replay "$trades" "$depth" BTC-PERPETUAL 0 active_l2 1 prorata \
      "${STRATEGY_ARGS[@]}" "--maker-fee-rate=$rate" 2>&1)
    fills=$(field_of "$output" trade_reports)
    net=$(field_of "$output" net_pnl_usd)
    gross=$(field_of "$output" gross_pnl_usd)
    closed=$(field_of "$output" closed_pnl_usd)
    per_fill=$(field_of "$output" gross_closed_per_fill_usd)
    breakeven=$(field_of "$output" breakeven_maker_rate)
    table+="| $name | $rate | $fills | $net | $gross | $closed | $per_fill | $breakeven |"$'\n'
    block+="run: l2_replay $trades $depth BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=$rate"$'\n'
    block+="then: trade_reports=$fills net_pnl_usd=$net gross_pnl_usd=$gross closed_pnl_usd=$closed gross_closed_per_fill_usd=$per_fill breakeven_maker_rate=$breakeven"$'\n'
  done
done

printf '%s\n' "$table"
echo "<!-- toyquant:check"
printf '%s' "$block"
echo "-->"
