#!/usr/bin/env bash
#
# Tiered behaviour check for the L2 replay path.
#
# The full-file comparison is the strong check, but it reads 1.5 GB, so it is not worth running on
# every edit. This splits the checks into what a change has to pass immediately and what it has to
# pass before it is accepted:
#
#   tier 0  (~0.2 s) the unit tests, because a red test must not be able to pass this gate
#   tier 1  (~1 s)   L2 200k slice and L1 invariants: batch counts, orders, fills, PnL, queue ahead
#   tier 2  (~15 s)  full 1.5 GB stdout hash against a cached reference
#
# Tier 1 catches essentially everything a reader or book change can get wrong, because it compares
# event counts, order and fill counts, realised PnL and consumed queue position. Tier 2 is what makes
# a timing claim defensible: it rules out every explanation except cost. Tier 0 was added after a
# commit landed carrying a failing assertion: this script compared replay outputs only, so the unit
# tests could be red while the gate reported success.
#
# Usage:
#   tools/verify_l2.sh fast [binary]   # tier 0 and tier 1, the per-edit loop
#   tools/verify_l2.sh ref  [binary]   # store the current full-file output as the reference
#   tools/verify_l2.sh full [binary]   # tier 2 only, against the stored reference
#   tools/verify_l2.sh all  [binary]   # all tiers, for accepting a change
#
# Binary defaults to out/build/linux-debug/toy_quant. Output is one line per tier on success, and
# the expected/actual values on failure. Exit status is 0 when every requested tier matched.

set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${2:-out/build/linux-debug/toy_quant}"
DOC_CHECK_LOG="$(mktemp)"
FIELD_CHECK_LOG="$(mktemp)"
PROSE_CHECK_LOG="$(mktemp)"
TRADES=data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
L1_TRADES=data/v2/test_aggTrades_5k.csv
L1_BBO=data/v2/test_bookTicker_5k.csv
FULL_DEPTH=data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv
SLICE_200K=/tmp/depth_200k_base.csv
SLICE_2M=/tmp/depth_2m.csv
REF_HASH=/tmp/l2_full_ref.sha256

# The slices are cut from the tracked depth file. /tmp is volatile, so cut them again on demand
# rather than failing with a confusing "input file does not exist".
ensure_slices() {
  [ -f "$SLICE_200K" ] || head -n 200001 "$FULL_DEPTH" > "$SLICE_200K"
  [ -f "$SLICE_2M" ] || head -n 2000001 "$FULL_DEPTH" > "$SLICE_2M"
}

# Sort the tokens so the comparison does not depend on the order the summary lines happen to print in.
tokens() { tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' '; }

L2_WANT=$(printf '%s' "incremental_batches=117288 submitted_orders=406 trade_reports=1 queue_ahead_consumed=55460 realized_pnl=-0.002 equity=1000" | tokens)
L1_WANT=$(printf '%s' "submitted_orders=13734 trade_reports=2277 fill_rate=0.175872 realized_pnl=-33.7861 equity=966.196" | tokens)

tier0() {
  local build_dir log
  build_dir=$(dirname "$BIN")
  log=$(ctest --test-dir "$build_dir" 2>&1 | grep -E 'tests passed')
  if printf '%s' "$log" | grep -q '100% tests passed'; then
    echo "tier0 OK"
    return 0
  fi
  echo "tier0 MISMATCH"
  echo "  ${log:-ctest produced no result; is $build_dir built?}"
  ctest --test-dir "$build_dir" --rerun-failed --output-on-failure 2>&1 | tail -20 | sed 's/^/  /'
  return 1
}

tier1() {
  local l2 l1 rc=0
  # Every metric field a summary prints has to be written somewhere. A field that is read but never
  # written looks exactly like a field whose value is zero, and this project shipped four of those in
  # one line before. Source-level, so it costs nothing and runs on every edit.
  local fields_check=0
  python3 tools/check_written_fields.py > "$FIELD_CHECK_LOG" 2>&1 || fields_check=1
  # Prose is not covered by check_docs.py, which only reads marked number blocks, and a paragraph that
  # describes a stale model set is exactly what that leaves behind. This compares the claims against the
  # source: flags, queue models and tool paths.
  local prose_check=0
  python3 tools/lint_prose.py docs/PERFORMANCE_HISTORY.md docs/ARCHITECTURE.md docs/USER_GUIDE.md \
      docs/PERFORMANCE.md docs/LEARNING.md docs/DESIGN_REVIEW.md docs/index.md \
      > "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
  l2=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 conservative \
        --fast-validation 2>&1 | grep -oE 'incremental_batches=117288|submitted_orders=406|trade_reports=1|queue_ahead_consumed=55460|realized_pnl=-0.002|equity=1000' | tokens)
  l1=$("$BIN" replay "$L1_TRADES" "$L1_BBO" BTCUSDT 0 optimized 1000000 2>&1 |
        grep -oE 'submitted_orders=13734|trade_reports=2277|fill_rate=0.175872|realized_pnl=-33.7861|equity=966.196' | tokens)
  [ "$l2" = "$L2_WANT" ] || rc=1
  [ "$l1" = "$L1_WANT" ] || rc=1
  # The numbers the documents quote are claims about this code, and hand-copied claims drift: one did,
  # a "thirty times per fill" that a re-measurement turned into ten. They are checked at the same gate
  # as the invariants, on the same slices, so a documented value that stops being true fails a run.
  local doc_check=0
  python3 tools/check_docs.py --binary "$BIN" docs/PERFORMANCE_HISTORY.md docs/ARCHITECTURE.md \
    docs/DESIGN_REVIEW.md > "$DOC_CHECK_LOG" 2>&1 || doc_check=1
  if [ $rc -eq 0 ] && [ $doc_check -eq 0 ] && [ $fields_check -eq 0 ] && [ $prose_check -eq 0 ]; then
    echo "tier1 OK"
    return 0
  fi
  echo "tier1 MISMATCH"
  [ $fields_check -eq 0 ] || sed 's/^/  fields: /' "$FIELD_CHECK_LOG"
  [ $prose_check -eq 0 ] || sed 's/^/  prose: /' "$PROSE_CHECK_LOG"
  [ "$l2" = "$L2_WANT" ] || { echo "  L2 want: $L2_WANT"; echo "  L2 got : $l2"; }
  [ "$l1" = "$L1_WANT" ] || { echo "  L1 want: $L1_WANT"; echo "  L1 got : $l1"; }
  [ $doc_check -eq 0 ] || sed 's/^/  doc: /' "$DOC_CHECK_LOG"
  return 1
}

full_hash() {
  "$BIN" l2_replay "$TRADES" "$FULL_DEPTH" BTC-PERPETUAL 0 active_l2 1 conservative \
    --fast-validation 2>&1 | sha256sum | cut -d' ' -f1
}

tier2() {
  if [ ! -f "$REF_HASH" ]; then
    echo "tier2 SKIP (no reference yet; run: tools/verify_l2.sh ref)"
    return 0
  fi
  local now
  now=$(full_hash)
  if [ "$now" = "$(cat "$REF_HASH")" ]; then
    echo "tier2 OK"
    return 0
  fi
  echo "tier2 MISMATCH"
  echo "  reference: $(cat "$REF_HASH")"
  echo "  current  : $now"
  return 1
}

case "${1:-fast}" in
  fast) ensure_slices; rc=0; tier0 || rc=1; tier1 || rc=1; exit $rc ;;
  ref)  ensure_slices; full_hash > "$REF_HASH"; echo "reference stored: $(cat "$REF_HASH")" ;;
  full) ensure_slices; tier2 ;;
  all)  ensure_slices; rc=0; tier0 || rc=1; tier1 || rc=1; tier2 || rc=1; exit $rc ;;
  *)    echo "usage: tools/verify_l2.sh fast|ref|full|all [binary]" >&2; exit 2 ;;
esac
