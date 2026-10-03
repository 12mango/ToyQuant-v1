#!/usr/bin/env bash
#
# Re-run the F4 and equity windows: the markets where the fee is smaller than a tick, measured with the decision
# layer the perpetuals were measured with, and print what each one earned.
#
# The perpetuals could not answer the fee question because their fee was a multiple of a tick. These markets
# can: a micro E-mini S&P 500 future pays about 0.62 USD a contract against a tick worth 1.25, and a US share
# pays about 0.0035 USD against a tick of one cent. SPY is the window that put the question in play; AAPL and
# TSLA are the two symbols the pre-registered screen in docs/ROADMAP.md passed, and every US equity row is the
# same policy on the same two hours. All files are Databento mbp-10 downloads converted by
# tools/fetch_databento.py to the CSV pair the existing readers take, so this script needs those files to be
# present and says so rather than inventing numbers:
#
#   python3 tools/fetch_databento.py --download --dataset GLBX.MDP3 --schema mbp-10 --symbol MESZ6 \
#     --start 2026-09-29T13:30 --end 2026-09-29T15:30 --pretty-px --output /tmp/mes.csv
#   python3 tools/fetch_databento.py --convert --input /tmp/mes.csv --symbol MESZ6 --exchange glbx \
#     --depth-out data/v2/futures/glbx_MESZ6_depth_2026-09-29_1330_1530.csv.gz \
#     --trades-out data/v2/futures/glbx_MESZ6_trades_2026-09-29_1330_1530.csv.gz
#
# and the same pair for a US share, with --dataset XNAS.ITCH, --exchange xnas and --pretty-px, which is not
# optional: without it the price column is the vendor's 1e-9 scaled integer and the run dies with std::bad_alloc.
#
# Usage:
#   tools/f4_windows.sh [binary]
#
# Prints a markdown table, then the recorded blocks that tools/check_docs.py re-derives for the three headline
# rows, one per market. A row that reports zero fills is a result and not a failure: it says the book never came
# that far.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=${1:-out/build/linux-debug/toy_quant}
MES_T=data/v2/futures/glbx_MESZ6_trades_2026-09-29_1330_1530.csv.gz
MES_D=data/v2/futures/glbx_MESZ6_depth_2026-09-29_1330_1530.csv.gz
SPY_T=data/v2/equities/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz
SPY_D=data/v2/equities/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz
# The two symbols the pre-registered screen passed, from the same session, downloaded with --pretty-px so the
# price column is decimal rather than the vendor's 1e-9 scaled integer: an integer price makes the engine's
# ladder index overflow and the run dies with bad_alloc, which is what the first attempt at these two did.
AAPL_T=data/v2/equities/xnas_AAPL_trades_2026-09-29_1330_1530.csv.gz
AAPL_D=data/v2/equities/xnas_AAPL_depth_2026-09-29_1330_1530.csv.gz
TSLA_T=data/v2/equities/xnas_TSLA_trades_2026-09-29_1330_1530.csv.gz
TSLA_D=data/v2/equities/xnas_TSLA_depth_2026-09-29_1330_1530.csv.gz
for file in "$MES_T" "$MES_D" "$SPY_T" "$SPY_D" "$AAPL_T" "$AAPL_D" "$TSLA_T" "$TSLA_D"; do
  if [ ! -f "$file" ]; then
    echo "missing $file: this window re-derives from a paid download, see tools/fetch_databento.py" >&2
    exit 2
  fi
done

# name|symbol|trades|depth|queue model|extra flags
WINDOWS=(
  "mes_1tick|MESZ6|$MES_T|$MES_D|conservative|--order-size=5 --inventory-limit=20 --base-spread-ticks=1"
  "mes_2tick|MESZ6|$MES_T|$MES_D|conservative|--order-size=5 --inventory-limit=20 --base-spread-ticks=2"
  "mes_4tick|MESZ6|$MES_T|$MES_D|conservative|--order-size=5 --inventory-limit=20 --base-spread-ticks=4"
  "mes_gate|MESZ6|$MES_T|$MES_D|conservative|--order-size=5 --inventory-limit=20 --base-spread-ticks=1 --markout-cover"
  "spy_1tick|SPY|$SPY_T|$SPY_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "spy_2tick|SPY|$SPY_T|$SPY_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=2"
  "spy_gate|SPY|$SPY_T|$SPY_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --markout-cover"
  "spy_band|SPY|$SPY_T|$SPY_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --flatten-band=1000"
  "aapl_conservative|AAPL|$AAPL_T|$AAPL_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "aapl_prorata|AAPL|$AAPL_T|$AAPL_D|prorata|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "aapl_lumpy|AAPL|$AAPL_T|$AAPL_D|lumpy|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "aapl_optimistic|AAPL|$AAPL_T|$AAPL_D|optimistic|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "aapl_band|AAPL|$AAPL_T|$AAPL_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --flatten-band=300"
  "aapl_limit200|AAPL|$AAPL_T|$AAPL_D|conservative|--order-size=100 --inventory-limit=200 --base-spread-ticks=1"
  "aapl_derived|AAPL|$AAPL_T|$AAPL_D|conservative|--order-size=100 --base-spread-ticks=1"
  "tsla_conservative|TSLA|$TSLA_T|$TSLA_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "tsla_prorata|TSLA|$TSLA_T|$TSLA_D|prorata|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "tsla_lumpy|TSLA|$TSLA_T|$TSLA_D|lumpy|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "tsla_optimistic|TSLA|$TSLA_T|$TSLA_D|optimistic|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1"
  "tsla_band|TSLA|$TSLA_T|$TSLA_D|conservative|--order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --flatten-band=300"
  "tsla_limit200|TSLA|$TSLA_T|$TSLA_D|conservative|--order-size=100 --inventory-limit=200 --base-spread-ticks=1"
  "tsla_derived|TSLA|$TSLA_T|$TSLA_D|conservative|--order-size=100 --base-spread-ticks=1"
)
# The rows the per-edit gate re-derives: one per market, because each is a claim that has to stay true -- the
# window that put the fee question in play, and the two whose capture is the first to cover the fee. Each pinned
# row costs about three seconds, so the three of them are affordable inside the loop that runs on every edit;
# the other fifteen are one command away through this script.
PINNED="spy_1tick aapl_conservative tsla_conservative"

field_of() {
  printf '%s' "$1" | grep -oE "(^| )$2=[^ ]+" | head -n 1 | sed 's/^ //; s/^[A-Za-z_][A-Za-z0-9_]*=//'
}

echo "| window | fills | units | fee USD | capture ticks | markout ticks | gross USD | closed USD | net USD | net/fill USD | breakeven rate | peak inventory |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
table=""
block=""
for window in "${WINDOWS[@]}"; do
  IFS='|' read -r name symbol trades depth model flags <<<"$window"
  # shellcheck disable=SC2086
  output=$("$BIN" l2_replay "$trades" "$depth" "$symbol" 0 active_l2 1 "$model" $flags 2>&1)
  fills=$(field_of "$output" trade_reports)
  units=$(field_of "$output" trade_report_quantity)
  fees=$(field_of "$output" fees_paid)
  capture=$(field_of "$output" captured_edge_per_unit_ticks)
  markout=$(field_of "$output" markout_per_unit_ticks)
  gross=$(field_of "$output" gross_pnl_usd)
  closed=$(field_of "$output" closed_pnl_usd)
  net=$(field_of "$output" net_pnl_usd)
  per_fill=$(field_of "$output" net_per_fill_usd)
  breakeven=$(field_of "$output" breakeven_maker_rate)
  inventory=$(field_of "$output" max_abs_inventory)
  table+="| $name | $fills | $units | $fees | $capture | $markout | $gross | $closed | $net | $per_fill | $breakeven | $inventory |"$'\n'
  case " $PINNED " in
    *" $name "*)
      block+="run: l2_replay $trades $depth $symbol 0 active_l2 1 $model $flags"$'\n'
      block+="then: trade_reports=$fills trade_report_quantity=$units fees_paid=$fees captured_edge_per_unit_ticks=$capture markout_per_unit_ticks=$markout gross_pnl_usd=$gross closed_pnl_usd=$closed net_pnl_usd=$net net_per_fill_usd=$per_fill breakeven_maker_rate=$breakeven max_abs_inventory=$inventory"$'\n'
      ;;
  esac
done

printf '%s\n' "$table"
echo "<!-- toyquant:check"
printf '%s' "$block"
echo "-->"
