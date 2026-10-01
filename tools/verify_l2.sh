#!/usr/bin/env bash
#
# Tiered behaviour check for the L2 replay path.
#
# The full-file comparison is the strong check, but it reads 1.5 GB, so it is not worth running on
# every edit. This splits the checks into what a change has to pass immediately and what it has to
# pass before it is accepted:
#
#   tier 0  (~0.2 s) the unit tests, because a red test must not be able to pass this gate
#   tier 1  (~3 s, including a 2M-row replay behind one marked block)   L2 200k slice and L1 invariants: batch counts, orders, fills, PnL, queue ahead, and the record-and-replay and direct-versus-threaded round trips
#   tier 2  (~15 s)  full 1.5 GB stdout hash against a cached reference
#   tier 3  (~8 s)   the same tests and one short replay under ASan and UBSan
#
# Tier 1 catches essentially everything a reader or book change can get wrong, because it compares
# event counts, order and fill counts, realised PnL and consumed queue position. Tier 2 is what makes
# a timing claim defensible: it rules out every explanation except cost. Tier 0 was added after a
# commit landed carrying a failing assertion: this script compared replay outputs only, so the unit
# tests could be red while the gate reported success. Tier 3 is the one standard C++ check the project
# did not run: a leak in the block reader, an out-of-range slot in the ladder or a signed overflow in
# the parser is invisible in a normal build.
#
# Usage:
#   tools/verify_l2.sh fast [binary]   # tier 0 and tier 1, the per-edit loop
#   tools/verify_l2.sh ref  [binary]   # store the current full-file output as the reference
#   tools/verify_l2.sh full [binary]   # tier 2 only, against the stored reference
#   tools/verify_l2.sh asan            # tier 3 only, the sanitizer build
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
TOOL_CHECK_LOG="$(mktemp)"
TRADES=data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
L1_TRADES=data/v2/test_aggTrades_5k.csv
L1_BBO=data/v2/test_bookTicker_5k.csv
FULL_DEPTH=data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv
SLICE_200K=/tmp/depth_200k_base.csv
SLICE_2M=/tmp/depth_2m.csv
# The second day exists only as a gzipped snapshot file, so its slice is cut through zcat. It backs the
# out-of-sample check in docs/PERFORMANCE_HISTORY.md, whose numbers the gate re-derives. All three slices are
# built on demand, so the cut costs a second once and nothing afterwards.
SNAPSHOT_0501=data/v2/deribit_book_snapshot_25_2020-05-01_BTC-PERPETUAL.csv.gz
SLICE_0501=/tmp/snap_0501_200k.csv
# The funding block in docs/PERFORMANCE_HISTORY.md replays ten minutes around the 2024-03-28 08:00 UTC funding
# settlement, because a Binance day file is cut on UTC midnight and midnight is itself a settlement instant, so a
# window that begins with the day applies that settlement to an account that holds nothing. The two slices are one
# streamed pass each over the day files, cached in /tmp like the rest, and skipped when the day is not on disk.
BINANCE_TRADES_0328=data/v2/BTCUSDT-aggTrades-2024-03-28.csv
BINANCE_BBO_0328=data/v2/BTCUSDT-bookTicker-2024-03-28.csv
SLICE_SETTLE_TRADES=/tmp/btc_trades_settle.csv
SLICE_SETTLE_BBO=/tmp/btc_bbo_settle.csv
SETTLE_FROM=1711612500000
SETTLE_TO=1711613100000
REF_HASH=/tmp/l2_full_ref.sha256

# The slices are cut from the tracked depth file. /tmp is volatile, so cut them again on demand
# rather than failing with a confusing "input file does not exist".
ensure_slices() {
  [ -f "$SLICE_200K" ] || head -n 200001 "$FULL_DEPTH" > "$SLICE_200K"
  [ -f "$SLICE_2M" ] || head -n 2000001 "$FULL_DEPTH" > "$SLICE_2M"
  if [ ! -f "$SLICE_0501" ] && [ -f "$SNAPSHOT_0501" ]; then
    zcat "$SNAPSHOT_0501" 2>/dev/null | head -n 200001 > "$SLICE_0501"
  fi
  # The out-of-sample table in docs/PERFORMANCE_HISTORY.md quotes one replay per day, each against a slice of that
  # day, and the blocks name the slice. Cutting them here means a day whose slice is missing fails with a run rather
  # than a confusing "input file does not exist", for the same reason as the three above.
  for day_file in data/v2/deribit_book_snapshot_25_*_BTC-PERPETUAL.csv data/v2/deribit_book_snapshot_25_*_BTC-PERPETUAL.csv.gz; do
    [ -e "$day_file" ] || continue
    case "$day_file" in
      *2020-05-01*) continue ;;
    esac
    day=$(printf '%s' "$day_file" | sed -n 's/.*_\([0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\)_BTC-PERPETUAL\.csv\(\.gz\)\{0,1\}$/\1/p')
    [ -n "$day" ] || continue
    day_slice="/tmp/snap_$(printf '%s' "$day" | tr -d -)_200k.csv"
    if [ ! -f "$day_slice" ]; then
      case "$day_file" in
        *.gz) zcat "$day_file" | head -n 200001 > "$day_slice" ;;
        *) head -n 200001 "$day_file" > "$day_slice" ;;
      esac
    fi
  done
  # The two slices the funding block names, cut from the Binance day files the same way and skipped when those are
  # not on disk. The window starts five minutes before the 08:00 UTC settlement and ends five minutes after it, so
  # the replay is holding a position when the settlement arrives; the cut is one pass per file through the tool the
  # document names, rather than an inline filter that a reader would have to trust.
  if [ ! -f "$SLICE_SETTLE_TRADES" ] && [ -f "$BINANCE_TRADES_0328" ]; then
    python3 tools/slice_csv_window.py --input "$BINANCE_TRADES_0328" --column 6 \
      --from "$SETTLE_FROM" --to "$SETTLE_TO" --output "$SLICE_SETTLE_TRADES" > /dev/null
  fi
  if [ ! -f "$SLICE_SETTLE_BBO" ] && [ -f "$BINANCE_BBO_0328" ]; then
    python3 tools/slice_csv_window.py --input "$BINANCE_BBO_0328" --column 6 \
      --from "$SETTLE_FROM" --to "$SETTLE_TO" --output "$SLICE_SETTLE_BBO" > /dev/null
  fi
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
      docs/PERFORMANCE.md docs/LEARNING.md docs/DESIGN_REVIEW.md docs/LATENCY_DESIGN.md docs/index.md \
      docs/ROADMAP.md > "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
  l2=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 conservative \
        --fast-validation 2>&1 | grep -oE 'incremental_batches=117288|submitted_orders=406|trade_reports=1|queue_ahead_consumed=55460|realized_pnl=-0.002|equity=1000' | tokens)
  l1=$("$BIN" replay "$L1_TRADES" "$L1_BBO" BTCUSDT 0 optimized 1000000 2>&1 |
        grep -oE 'submitted_orders=13734|trade_reports=2277|fill_rate=0.175872|realized_pnl=-33.7861|equity=966.196' | tokens)
  [ "$l2" = "$L2_WANT" ] || rc=1
  [ "$l1" = "$L1_WANT" ] || rc=1
  # The recorder's assertion, which is what the recorder half of Stage A claims: the stream a replay
  # delivered, written back out in the replay shape and replayed again, is the same run. The
  # comparison drops the two kinds of line that have to differ, the input paths in the [Mode] line
  # and the [REC] line that only the recording run prints, and requires every other number to match
  # exactly: event counts, orders, fills, the queue that was consumed, PnL and the final equity.
  local rec_ok=0 rec_prefix=out/lat/rec_gate
  mkdir -p out/lat
  # The recording run is paced at 200x, so the assertion covers the claim as Stage A words it — a
  # paced session becomes a replayable file — while the schedule stays a fraction of a second of wall
  # clock and the tier never waits out the window. The replay of the recording is unpaced, because
  # what is being compared is the decisions, not the sleeps.
  rec_a=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --live-pacing=200 --record="$rec_prefix" 2>&1 | grep -v -e '^\[Mode:' -e '^\[REC\]' -e '^\[LIVE\]')
  rec_b=$("$BIN" l2_replay "${rec_prefix}_trades.csv" "${rec_prefix}_depth.csv" BTC-PERPETUAL 0 active_l2 1 \
        prorata --fast-validation 2>&1 | grep -v -e '^\[Mode:' -e '^\[REC\]' -e '^\[LIVE\]')
  [ "$rec_a" = "$rec_b" ] || rec_ok=1
  # The threading assertion: the same slice through a feed thread and a ring has to report the same
  # numbers as the direct path, because threading changes when a decision is taken and not what it is.
  # The [LATENCY] line is allowed to differ, being the measurement rather than the result, and it has
  # to carry exactly one hand-off per delivered event, which is checked against the run's own event
  # count instead of a number copied out of this script.
  local thr_ok=0 thr_raw thr plain
  thr_raw=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --feed-thread 2>&1)
  plain=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        2>&1 | grep -v '^\[Mode:')
  thr=$(printf '%s' "$thr_raw" | grep -v -e '^\[Mode:' -e '^\[LATENCY\]')
  [ "$thr" = "$plain" ] || thr_ok=1
  thr_events=$(printf '%s' "$thr_raw" | grep -oE 'events=[0-9]+' | head -1 | sed 's/events=//')
  thr_handoffs=$(printf '%s' "$thr_raw" | grep -oE '^\[LATENCY\] handoffs=[0-9]+' | sed 's/.*=//')
  [ -n "$thr_events" ] && [ "$thr_events" = "$thr_handoffs" ] || thr_ok=1
  # The broker sits between the strategy and the engine, so a broker with no faults configured has to
  # produce the run without it, apart from the two lines that are the broker and the reconciliation, and
  # the reconciliation has to agree with the portfolio. A faulted broker, on the other hand, has to be
  # visible: a fault injection whose faults cannot be seen is worse than none, because the run carrying it
  # looks exactly like a clean one.
  local broker_ok=0 broker_raw broker_plain broker_break
  broker_raw=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --paper-broker 2>&1)
  broker_plain=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        2>&1 | grep -v '^\[Mode:')
  [ "$(printf '%s' "$broker_raw" | grep -v -e '^\[Mode:' -e '^\[BROKER\]' -e '^\[RECON\]')" = "$broker_plain" ] ||
    broker_ok=1
  printf '%s' "$broker_raw" | grep -q '^\[RECON\].*status=ok' || broker_ok=1
  printf '%s' "$broker_raw" | grep -q '^\[RECON\].*quantity_breaks=0 max_quantity_break=0 net_quantity_break=0 gross_quantity_break=0' ||
    broker_ok=1
  printf '%s' "$broker_raw" | grep -q '^\[BROKER\].*orders_rejected=0 fills_seen=[0-9]* fills_split=0 fills_dropped=0' ||
    broker_ok=1
  # One lost execution report has to show up as a break, in the same columns the check above reads as
  # zero. Which column it lands in depends on the fills the window happens to have, so only the verdict
  # is asserted here.
  broker_break=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --paper-broker --broker-drop-every=1 2>&1 | grep -oE '^\[RECON\].*status=[a-z]+$')
  printf '%s' "$broker_break" | grep -q 'status=break' || broker_ok=1
  # The numbers the documents quote are claims about this code, and hand-copied claims drift: one did,
  # a "thirty times per fill" that a re-measurement turned into ten. They are checked at the same gate
  # as the invariants, on the same slices, so a documented value that stops being true fails a run.
  local doc_check=0
  python3 tools/check_docs.py --binary "$BIN" docs/PERFORMANCE_HISTORY.md docs/ARCHITECTURE.md \
    docs/DESIGN_REVIEW.md docs/PERFORMANCE.md > "$DOC_CHECK_LOG" 2>&1 || doc_check=1
  # The IEX HIST reader walks the pcap, IP, UDP and IEX-TP framing that a decoder sits on top of. The file it
  # walks in this test is built by the tool rather than downloaded, because the archive is behind an account,
  # so the container walk is the part that can be checked on every edit. It says nothing about DEEP messages,
  # which is why the tool decodes none.
  local tool_check=0
  python3 tools/fetch_iex_hist.py --self-test > "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  if [ $rc -eq 0 ] && [ $rec_ok -eq 0 ] && [ $thr_ok -eq 0 ] && [ $broker_ok -eq 0 ] &&
     [ $doc_check -eq 0 ] &&
     [ $fields_check -eq 0 ] && [ $prose_check -eq 0 ] && [ $tool_check -eq 0 ]; then
    echo "tier1 OK"
    return 0
  fi
  echo "tier1 MISMATCH"
  [ $rec_ok -eq 0 ] || echo "  recorder: replaying the recording did not reproduce the run"
  [ $thr_ok -eq 0 ] || echo "  threads: the threaded run did not match the direct run, or its hand-off count did not"
  [ $broker_ok -eq 0 ] || echo "  broker: the brokered run did not match the direct one, or a lost report was not reported"
  [ $fields_check -eq 0 ] || sed 's/^/  fields: /' "$FIELD_CHECK_LOG"
  [ $prose_check -eq 0 ] || sed 's/^/  prose: /' "$PROSE_CHECK_LOG"
  [ $tool_check -eq 0 ] || sed 's/^/  tool: /' "$TOOL_CHECK_LOG"
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

# Tier 3 sanitizes the code that the other tiers only measure. A leak in the block reader, an out-of-range
# slot in the ladder or a signed overflow in the parser is invisible in a normal build. The build is
# incremental, so the cost is a rebuild of whatever changed plus a few seconds of running, and
# -fno-sanitize-recover=all makes the first undefined behaviour abort instead of warning, so a finding cannot
# be missed by reading the log too quickly. If the toolchain has no sanitizers the tier skips rather than
# failing: a missing compiler feature is not a regression in this code.
tier3() {
  local asan_build_dir=out/build/linux-asan
  local log
  log=$(mktemp)
  if ! cmake --preset linux-asan > "$log" 2>&1; then
    echo "tier3 SKIP (the sanitizer preset did not configure on this toolchain)"
    return 0
  fi
  if ! cmake --build "$asan_build_dir" >> "$log" 2>&1; then
    echo "tier3 MISMATCH (sanitizer build failed)"
    tail -20 "$log" | sed 's/^/  /'
    return 1
  fi
  local rc=0
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1" \
    ctest --test-dir "$asan_build_dir" >> "$log" 2>&1 || rc=1
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1" \
    "$asan_build_dir/toy_quant" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata \
    --fast-validation --no-output >> "$log" 2>&1 || rc=1
  local finding
  finding=$(grep -m3 -E 'ERROR: AddressSanitizer|runtime error:|LeakSanitizer' "$log")
  if [ $rc -eq 0 ] && [ -z "$finding" ]; then
    echo "tier3 OK"
    return 0
  fi
  echo "tier3 MISMATCH"
  [ -z "$finding" ] || echo "$finding" | sed 's/^/  /'
  [ $rc -eq 0 ] || tail -5 "$log" | sed 's/^/  /'
  return 1
}

case "${1:-fast}" in
  fast) ensure_slices; rc=0; tier0 || rc=1; tier1 || rc=1; exit $rc ;;
  ref)  ensure_slices; full_hash > "$REF_HASH"; echo "reference stored: $(cat "$REF_HASH")" ;;
  full) ensure_slices; tier2 ;;
  asan) ensure_slices; tier3 ;;
  all)  ensure_slices; rc=0; tier0 || rc=1; tier1 || rc=1; tier2 || rc=1; tier3 || rc=1; exit $rc ;;
  *)    echo "usage: tools/verify_l2.sh fast|ref|full|asan|all [binary]" >&2; exit 2 ;;
esac
