#!/usr/bin/env bash
# Reproduce the measured lever matrix in one command.
#
# The fill count has been this project's central open question, and four levers move it: quote placement,
# the requote threshold, the queue model, and the arrival share. This runs each lever around the same
# window and prints the numbers that decide whether the fills were worth having, so a comparison is
# reproduced rather than remembered. The measured ordering of the levers, from PERFORMANCE_HISTORY.md:
# requote 7.7x, queue model 5x, placement 3x, arrival share under 2x.
#
# Usage:
#   tools/workload_matrix.sh [placement|requote|model|arrival|all]
#
# TRADES and DEPTH override the inputs; the slice is cut by tools/verify_l2.sh.
set -u

TRADES="${TRADES:-data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz}"
DEPTH="${DEPTH:-/tmp/depth_200k_base.csv}"
export TOY_QUANT_FIELDS="${TOY_QUANT_FIELDS:-submitted_orders trade_reports fill_rate quote_at_touch_orders captured_edge_per_unit_ticks markout_per_unit_ticks captured_edge_usd fees_paid realized_pnl}"

BASE="l2_replay $TRADES $DEPTH BTC-PERPETUAL 0 active_l2 1"
FAST="--fast-validation"
TOUCH="$FAST --base-spread-ticks=1"
HOLD="$TOUCH --refresh-price-ticks=4"

case "${1:-all}" in
    placement)
        bash tools/compare_runs.sh "at the touch" "$BASE prorata $TOUCH" \
                                   "2 ticks (default)" "$BASE prorata $FAST" \
                                   "4 ticks" "$BASE prorata $FAST --base-spread-ticks=4"
        ;;
    requote)
        bash tools/compare_runs.sh "refresh 2 (default)" "$BASE prorata $TOUCH" \
                                   "refresh 4" "$BASE prorata $TOUCH --refresh-price-ticks=4" \
                                   "refresh 8" "$BASE prorata $TOUCH --refresh-price-ticks=8"
        ;;
    model)
        bash tools/compare_runs.sh "conservative" "$BASE conservative $HOLD" \
                                   "prorata" "$BASE prorata $HOLD" \
                                   "lumpy k=1" "$BASE lumpy $HOLD" \
                                   "optimistic" "$BASE optimistic $HOLD"
        ;;
    arrival)
        bash tools/compare_runs.sh "alpha 1.0 (FIFO)" "$BASE prorata $HOLD" \
                                   "alpha 0.5" "$BASE prorata $HOLD --arrival-share=0.5" \
                                   "alpha 0.0" "$BASE prorata $HOLD --arrival-share=0.0"
        ;;
    all)
        for lever in placement requote model arrival; do
            printf '\n### %s\n\n' "$lever"
            "$0" "$lever"
        done
        ;;
    *)
        echo "usage: tools/workload_matrix.sh [placement|requote|model|arrival|all]" >&2
        exit 2
        ;;
esac
