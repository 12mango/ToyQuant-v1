#!/usr/bin/env bash
# Compare replay runs field by field, and emit the comparison as a checkable markdown table.
#
# The comparison this project keeps making is "same data, one thing changed", and the answer is always
# buried in two long [EXECUTION] lines that differ in a handful of fields. This runs each variant and
# prints one row per field and one column per variant.
#
# Usage:
#   tools/compare_runs.sh [--markdown] <label> <args...> [<label> <args...> ...]
#
# With --markdown the output is a table to paste into docs/, preceded by a comment block that
# tools/check_docs.py reads back. Pasting the output is therefore the whole job of documenting a
# comparison, and tools/check_docs.py will fail the build when the quoted numbers stop being true.
#
# Example: the queue models on one window
#   TRADES=data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
#   DEPTH=/tmp/depth_200k_base.csv
#   tools/compare_runs.sh --markdown \
#     conservative "l2_replay $TRADES $DEPTH BTC-PERPETUAL 0 active_l2 1 conservative --fast-validation" \
#     prorata      "l2_replay $TRADES $DEPTH BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation"
#
# TOY_QUANT_BIN overrides the binary, TOY_QUANT_FIELDS the columns.
set -u

BIN="${TOY_QUANT_BIN:-out/build/linux-debug/toy_quant}"
FIELDS="${TOY_QUANT_FIELDS:-submitted_orders trade_reports fill_rate queue_ahead_consumed buy_queue_from_quantity_changes sell_queue_from_quantity_changes captured_edge_per_unit_ticks markout_per_unit_ticks markout_count max_abs_inventory max_abs_exposure_usd exposure_over_collateral_fills strategy_position_mismatches position_limit risk_rejected_orders realized_pnl equity fees_paid working_orders}"

markdown=0
if [[ "${1:-}" == "--markdown" ]]; then
    markdown=1
    shift
fi

if [[ $# -lt 2 || $(( $# % 2 )) -ne 0 ]]; then
    grep '^# ' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

labels=()
commands=()
outputs=()
while [[ $# -gt 0 ]]; do
    labels+=("$1")
    commands+=("$2")
    shift 2
done

index=0
while [[ $index -lt ${#commands[@]} ]]; do
    # shellcheck disable=SC2086
    outputs+=("$("$BIN" ${commands[$index]} 2>&1 | tr '\n' ' ')")
    index=$(( index + 1 ))
done

value_of() {
    printf '%s' "$1" | grep -oE "(^| )$2=[^ ]+" | head -1 | cut -d= -f2-
}

if [[ $markdown -eq 1 ]]; then
    printf '<!-- toyquant:check\n'
    index=0
    while [[ $index -lt ${#commands[@]} ]]; do
        printf 'run: %s\n' "${commands[$index]}"
        printf 'then:'
        for field in $FIELDS; do
            value=$(value_of "${outputs[$index]}" "$field")
            [[ -n "$value" ]] && printf ' %s=%s' "$field" "$value"
        done
        printf '\n'
        index=$(( index + 1 ))
    done
    printf '%s\n' '-->'
    printf '| field |'
    for label in "${labels[@]}"; do printf ' %s |' "$label"; done
    printf '\n|---|'
    for label in "${labels[@]}"; do printf '%s|' "---:"; done
    printf '\n'
    for field in $FIELDS; do
        printf '| `%s` |' "$field"
        index=0
        while [[ $index -lt ${#outputs[@]} ]]; do
            value=$(value_of "${outputs[$index]}" "$field")
            printf ' %s |' "${value:--}"
            index=$(( index + 1 ))
        done
        printf '\n'
    done
    exit 0
fi

printf '%-34s' "field"
for label in "${labels[@]}"; do printf ' %22s' "$label"; done
printf '\n'
for field in $FIELDS; do
    printf '%-34s' "$field"
    for output in "${outputs[@]}"; do
        value=$(value_of "$output" "$field")
        printf ' %22s' "${value:--}"
    done
    printf '\n'
done

if printf '%s' "${outputs[0]}" | grep -qE 'error|Error|Usage'; then
    printf 'first variant did not produce a run: %s\n' "$(printf '%s' "${outputs[0]}" | cut -c1-160)"
fi
