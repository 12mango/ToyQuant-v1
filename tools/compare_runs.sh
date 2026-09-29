#!/usr/bin/env bash
# Compare replay runs field by field.
#
# The comparison this project keeps making is "same data, one thing changed", and the answer is always
# buried in two long [EXECUTION] lines that differ in a handful of fields. This runs each variant and
# prints one row per field and one column per variant, so a difference is visible without diffing whole
# lines, and a run that fails prints its error instead of an empty row.
#
# Usage:
#   tools/compare_runs.sh <label> <args...> [<label> <args...> ...]
#
# Example: the queue models on one window
#   TRADES=data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
#   DEPTH=/tmp/depth_200k_base.csv
#   tools/compare_runs.sh \
#     conservative "l2_replay $TRADES $DEPTH BTC-PERPETUAL 0 active_l2 1 conservative --fast-validation" \
#     prorata      "l2_replay $TRADES $DEPTH BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation"
#
# TOY_QUANT_BIN overrides the binary, TOY_QUANT_FIELDS the columns.
set -u

BIN="${TOY_QUANT_BIN:-out/build/linux-debug/toy_quant}"
FIELDS="${TOY_QUANT_FIELDS:-submitted_orders trade_reports fill_rate queue_ahead_consumed buy_queue_from_quantity_changes sell_queue_from_quantity_changes captured_edge_per_unit_ticks markout_per_unit_ticks markout_count max_abs_inventory max_abs_exposure_usd exposure_over_collateral_fills strategy_position_mismatches position_limit risk_rejected_orders realized_pnl equity fees_paid working_orders}"

if [[ $# -lt 2 || $(( $# % 2 )) -ne 0 ]]; then
    grep '^# ' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

labels=()
outputs=()
while [[ $# -gt 0 ]]; do
    labels+=("$1")
    command=$2
    shift 2
    # shellcheck disable=SC2086
    outputs+=("$("$BIN" $command 2>&1 | tr '\n' ' ')")
done

printf '%-34s' "field"
for label in "${labels[@]}"; do printf ' %22s' "$label"; done
printf '\n'
for field in $FIELDS; do
    printf '%-34s' "$field"
    for output in "${outputs[@]}"; do
        value=$(printf '%s' "$output" | grep -oE "(^| )$field=[^ ]+" | head -1 | cut -d= -f2-)
        if [[ -n "$value" ]]; then
            printf ' %22s' "$value"
        elif printf '%s' "$output" | grep -q "$field"; then
            printf ' %22s' "(no value)"
        else
            printf ' %22s' "-"
        fi
    done
    printf '\n'
    # A run that failed produced no [EXECUTION] line at all; say so once instead of printing dashes.
    if printf '%s' "${outputs[0]}" | grep -qE 'error|Error|Usage'; then
        printf '  first variant did not produce a run: %s\n' "$(printf '%s' "${outputs[0]}" | cut -c1-120)"
        break
    fi
done
