#!/usr/bin/env bash
#
# Sweep the latency the same two windows do or do not tolerate, and print what each setting earned.
#
# The positive SPY row in the performance history was measured with a synchronous engine, which is the one
# assumption a retail account cannot make. This script varies the two latencies the engine models, order and
# cancel, at one and five and twenty and fifty milliseconds one way, on the same window and the same policy, and
# then on a slower policy as a control: if a quote that lives longer is less exposed to latency, the slow rows
# should be the survivors rather than the fast one.
#
# The windows are the converted Databento files from phase 2b, so this script needs them present and says so
# rather than inventing numbers:
#
#   python3 tools/fetch_databento.py --download --dataset XNAS.ITCH --schema mbp-10 --symbol SPY \
#     --start 2026-09-29T13:30 --end 2026-09-29T15:30 --pretty-px --output /tmp/spy.csv
#   python3 tools/fetch_databento.py --convert --input /tmp/spy.csv --symbol SPY --exchange xnas \
#     --depth-out data/v2/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz \
#     --trades-out data/v2/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz
#
# Usage:
#   tools/latency_sweep.sh [binary]
#
# Prints a markdown table, then the recorded blocks that tools/check_docs.py re-derives for the two decisive
# rows. Re-deriving the whole table costs about a minute and a half, which is too much for the per-edit loop, so
# only the crossing and the collapse are pinned.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=${1:-out/build/linux-debug/toy_quant}
MES_T=data/v2/glbx_MESZ6_trades_2026-09-29_1330_1530.csv.gz
MES_D=data/v2/glbx_MESZ6_depth_2026-09-29_1330_1530.csv.gz
SPY_T=data/v2/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz
SPY_D=data/v2/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz
for file in "$MES_T" "$MES_D" "$SPY_T" "$SPY_D"; do
  if [ ! -f "$file" ]; then
    echo "missing $file: this sweep re-derives from a paid download, see tools/fetch_databento.py" >&2
    exit 2
  fi
done

SPY_SIZING="--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
MES_SIZING="--order-size=5 --inventory-limit=20 --base-spread-ticks=1"
# name|symbol|trades|depth|flags|order_us|cancel_us
ROWS=(
  "spy fast, synchronous|SPY|$SPY_T|$SPY_D|$SPY_SIZING|0|0"
  "spy fast, 1 ms each way|SPY|$SPY_T|$SPY_D|$SPY_SIZING|1000|1000"
  "spy fast, 5 ms each way|SPY|$SPY_T|$SPY_D|$SPY_SIZING|5000|5000"
  "spy fast, 20 ms each way|SPY|$SPY_T|$SPY_D|$SPY_SIZING|20000|20000"
  "spy fast, 50 ms each way|SPY|$SPY_T|$SPY_D|$SPY_SIZING|50000|50000"
  "spy fast, 20 ms order only|SPY|$SPY_T|$SPY_D|$SPY_SIZING|20000|0"
  "spy fast, 20 ms cancel only|SPY|$SPY_T|$SPY_D|$SPY_SIZING|0|20000"
  "spy slow refresh 8, synchronous|SPY|$SPY_T|$SPY_D|$SPY_SIZING --refresh-price-ticks=8|0|0"
  "spy slow refresh 8, 5 ms each way|SPY|$SPY_T|$SPY_D|$SPY_SIZING --refresh-price-ticks=8|5000|5000"
  "spy slow refresh 16, synchronous|SPY|$SPY_T|$SPY_D|$SPY_SIZING --refresh-price-ticks=16|0|0"
  "spy slow refresh 8, two ticks wide, 5 ms each way|SPY|$SPY_T|$SPY_D|--order-size=100 --inventory-limit=1000 --base-spread-ticks=2 --refresh-price-ticks=8|5000|5000"
  "mes fast, synchronous|MESZ6|$MES_T|$MES_D|$MES_SIZING|0|0"
  "mes fast, 5 ms each way|MESZ6|$MES_T|$MES_D|$MES_SIZING|5000|5000"
)
# The two rows the gate re-derives: the setting at which the window goes to zero, and the setting at which it
# has given the price away.
PINNED="spy fast, 1 ms each way|spy fast, 5 ms each way"

field_of() {
  printf '%s' "$1" | grep -oE "(^| )$2=[^ ]+" | head -n 1 | sed 's/^ //; s/^[A-Za-z_][A-Za-z0-9_]*=//'
}

echo "| window | order/cancel | fills | shares | cancels | capture ticks | gross USD | net USD | net/fill USD | breakeven rate |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
block=""
for row in "${ROWS[@]}"; do
  IFS='|' read -r name symbol trades depth flags order_us cancel_us <<<"$row"
  # shellcheck disable=SC2086
  output=$("$BIN" l2_replay "$trades" "$depth" "$symbol" 0 active_l2 1 conservative $flags \
    --order-latency-us="$order_us" --cancel-latency-us="$cancel_us" 2>&1)
  fills=$(field_of "$output" trade_reports)
  shares=$(field_of "$output" trade_report_quantity)
  cancels=$(field_of "$output" cancel_requests)
  capture=$(field_of "$output" captured_edge_per_unit_ticks)
  gross=$(field_of "$output" gross_pnl_usd)
  net=$(field_of "$output" net_pnl_usd)
  per_fill=$(field_of "$output" net_per_fill_usd)
  breakeven=$(field_of "$output" breakeven_maker_rate)
  echo "| $name | $order_us/$cancel_us | $fills | $shares | $cancels | $capture | $gross | $net | $per_fill | $breakeven |"
  case "|$PINNED|" in
    *"|$name|"*)
      block+="run: l2_replay $trades $depth $symbol 0 active_l2 1 conservative $flags --order-latency-us=$order_us --cancel-latency-us=$cancel_us"$'\n'
      block+="then: trade_reports=$fills trade_report_quantity=$shares cancel_requests=$cancels captured_edge_per_unit_ticks=$capture gross_pnl_usd=$gross net_pnl_usd=$net net_per_fill_usd=$per_fill breakeven_maker_rate=$breakeven"$'\n'
      ;;
  esac
done

echo "<!-- toyquant:check"
printf '%s' "$block"
echo "-->"
