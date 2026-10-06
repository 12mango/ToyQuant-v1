#!/usr/bin/env bash
# Two sweeps over the six US equity windows already on disk, both costing nothing:
#
#   the kill switch, at four thresholds plus off, which is the measurement the commit of the switch does not
#   have: what it saves on the windows that go wrong against what it costs on the ones that recover, and how
#   long into the window it trips;
#
#   the exposure cap, at three levels plus off, which is the same question for the control that turned a
#   +6981.90 window into a -44.80 one.
#
# Every row is the pinned policy at the pinned size, so the sweep compares thresholds and caps rather than
# sizes. Run it from the repository root; it reads the paid windows and says so if one is missing.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=out/build/linux-relwithdebinfo/toy_quant
SCHED=--fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013
START_TS=1790688600000000000
FIELDS='trade_reports=[0-9]+|fees_paid=[-0-9.]+|net_pnl_usd=[-0-9.]+|max_abs_exposure_usd=[-0-9.e+]+|submitted_orders=[0-9]+|hard_limit_rejections=[0-9]+|kill_switch_triggered=[0-9]+|kill_switch_ts=[0-9]+|kill_switch_rejected_orders=[0-9]+'

WINDOWS=(AAPL TSLA SPY MSTR AMD MU)
for symbol in "${WINDOWS[@]}"; do
  for file in "data/v2/equities/xnas_${symbol}_trades_2026-09-29_1330_1530.csv.gz" \
              "data/v2/equities/xnas_${symbol}_depth_2026-09-29_1330_1530.csv.gz"; do
    if [ ! -f "$file" ]; then
      echo "missing $file: this sweep re-derives from paid downloads" >&2
      exit 2
    fi
  done
done

. tools/lib.sh

run() {
  # shellcheck disable=SC2086
  "$BIN" l2_replay "data/v2/equities/xnas_$1_trades_2026-09-29_1330_1530.csv.gz" \
    "data/v2/equities/xnas_$1_depth_2026-09-29_1330_1530.csv.gz" "$1" 0 active_l2 1 conservative \
    --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 $SCHED $2 2>&1
}

echo "| symbol | switch USD | tripped | at seconds | submissions | fills | fees USD | net USD |"
echo "|---|---:|---|---:|---:|---:|---:|---:|"
for symbol in "${WINDOWS[@]}"; do
  for limit in 0 250 500 1000 2000; do
    extra=""
    [ "$limit" != "0" ] && extra="--kill-switch-loss-usd=$limit"
    output=$(run "$symbol" "$extra")
    tripped=$(field_of "$output" kill_switch_triggered)
    at=$(field_of "$output" kill_switch_ts)
    seconds="-"
    if [ -n "$at" ] && [ "$at" != "0" ]; then
      seconds=$(( (at - START_TS) / 1000000000 ))
    fi
    printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' "$symbol" "$limit" "${tripped:-0}" "$seconds" \
      "$(field_of "$output" submitted_orders)" "$(field_of "$output" trade_reports)" \
      "$(field_of "$output" fees_paid)" "$(field_of "$output" net_pnl_usd)"
  done
done

echo
echo "| symbol | cap USD | submissions | refused | peak exposure USD | fills | fees USD | net USD |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|"
for symbol in MU AMD MSTR AAPL; do
  for cap in 0 100000 200000 350000; do
    extra=""
    [ "$cap" != "0" ] && extra="--max-exposure-usd=$cap"
    output=$(run "$symbol" "$extra")
    printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' "$symbol" "$cap" \
      "$(field_of "$output" submitted_orders)" "$(field_of "$output" hard_limit_rejections)" \
      "$(field_of "$output" max_abs_exposure_usd)" "$(field_of "$output" trade_reports)" \
      "$(field_of "$output" fees_paid)" "$(field_of "$output" net_pnl_usd)"
  done
done
echo "SWEEP DONE"
