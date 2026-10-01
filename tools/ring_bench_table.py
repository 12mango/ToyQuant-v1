#!/usr/bin/env python3
"""Run the ring hand-off benchmark several times and print the table the document quotes.

One session of a microbenchmark on a machine with a turbo clock is a sample, not a number: the same
two-thread row has moved by 1.5x between sessions on this laptop while the payload rows stayed within a
few per cent. So this runs `ring_bench` once per session, takes the median of the per-session medians for
each case, and prints the low and high session alongside, which is the uncertainty the table can state.

    python3 tools/ring_bench_table.py [--messages N] [--repetitions N] [--sessions N]

The output is a markdown table plus the deterministic line, which is what `docs/PERFORMANCE_HISTORY.md`
quotes and what its marked block re-derives through `tools/check_docs.py`.
"""

import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path

LINE = re.compile(r"^\[RING_BENCH\] (?P<body>.*)$", re.MULTILINE)

# The labels are the benchmark's own, in its order, because the document's table rows are these cases.
LABELS = {
    "empty_round_trip": "empty round trip, 1 thread, uint64",
    "full_ring_push": "full ring, refused push",
    "stream_uint64": "stream uint64, 2 threads, ring 4096",
    "stream_pod16": "stream 16 byte POD, prebuilt, 2 threads, ring 4096",
    "stream_pod128": "stream 128 byte POD, prebuilt, 2 threads, ring 4096",
    "stream_pod_event": "stream POD the event's size, prebuilt, 2 threads, ring 4096",
    "stream_trade_prebuilt": "stream trade event, prebuilt, 2 threads, ring 4096",
    "stream_trade": "stream trade event, built per message, 2 threads, ring 4096",
    "stream_batch": "stream book batch, built per message, 2 threads, ring 4096",
    "small_ring": "stream uint64, 2 threads, ring 8",
    "mutex_condvar": "stream trade event, mutex + condvar",
    "mutex_spin": "stream trade event, mutex, consumer spins",
}

# Fields of the [RING_BENCH] line that are a property of the code rather than of the machine.
DETERMINISTIC = ("messages", "repetitions", "checksum", "payload_bytes", "cpu")


def parse(line):
    values = {}
    for token in line.split():
        if "=" not in token:
            continue
        key, _, value = token.partition("=")
        values[key] = value
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path("out/build/linux-relwithdebinfo/ring_bench"))
    parser.add_argument("--messages", type=int, default=2000000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--sessions", type=int, default=3)
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"FAIL {args.binary} is not built; "
              f"cmake --build --preset linux-relwithdebinfo --target ring_bench")
        return 2

    sessions = []
    for session in range(args.sessions):
        completed = subprocess.run(
            [str(args.binary), str(args.messages), str(args.repetitions)],
            capture_output=True, text=True,
        )
        if completed.returncode != 0:
            print(f"FAIL session {session + 1} exited {completed.returncode}")
            print(completed.stderr.strip())
            return 2
        match = LINE.search(completed.stdout)
        if match is None:
            print(f"FAIL session {session + 1} printed no [RING_BENCH] line")
            return 2
        sessions.append(parse(match.group("body")))

    first = sessions[0]
    for field in DETERMINISTIC:
        if any(session.get(field) != first.get(field) for session in sessions):
            print(f"FAIL {field} differs between sessions: "
                  f"{[session.get(field) for session in sessions]}")
            return 1

    print(f"# ring hand-off, {first['messages']} messages per session, "
          f"{first['repetitions']} passes per case, median of {args.sessions} sessions")
    print(f"# cpu={first['cpu']} payload_bytes={first['payload_bytes']} checksum={first['checksum']}")
    print()
    print("| case | ns/op | session spread | M ops/s |")
    print("|---|---:|---:|---:|")
    for key, label in LABELS.items():
        if key not in first:
            continue
        values = [float(session[key]) for session in sessions]
        median = statistics.median(values)
        print(f"| {label} | {median:.2f} | {min(values):.1f}-{max(values):.1f} | "
              f"{1000.0 / median:.1f} |")
    print()
    print("deterministic: " + " ".join(f"{field}={first[field]}" for field in DETERMINISTIC))
    return 0


if __name__ == "__main__":
    sys.exit(main())
