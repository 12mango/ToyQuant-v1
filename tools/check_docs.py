#!/usr/bin/env python3
"""Verify that the numbers a document quotes are still what the code produces.

A document that quotes replay output is a claim about the code, and this project's claims are its
product. Hand-copied numbers drift: one already did, a "thirty times per fill" that a re-measurement
turned into ten. The comparison tables are therefore emitted by `tools/compare_runs.sh --markdown`,
which wraps them in a comment block carrying the exact command and the values it produced:

    <!-- toyquant:check
    run: l2_replay data/... 0 active_l2 1 conservative --fast-validation
    then: submitted_orders=406 trade_reports=1 captured_edge_per_unit_ticks=2.5
    -->

This runs each `run:` and fails when any `then:` value has moved. Only marked blocks are checked, so
historical prose is left alone and a table is checked exactly when someone decided it should be.

Usage:
    python3 tools/check_docs.py docs/PERFORMANCE_HISTORY.md [more.md ...]
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

MARKER = "<!-- toyquant:check"
END = "-->"
FIELD_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


def check_blocks(text):
    """Yields (line_number, [(command, {field: value}), ...]) for every marked block.

    Parsed line by line rather than with one regex over the whole file: the block spans lines, the
    obvious pattern was not matching what it looked like it should, and a marker line is easier to
    read than a pattern that has to be debugged.
    """
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        if lines[index].strip() != MARKER:
            index += 1
            continue
        body = []
        cursor = index + 1
        while cursor < len(lines) and lines[cursor].strip() != END:
            body.append(lines[cursor])
            cursor += 1
        yield index + 1, parse_block(body)
        index = cursor + 1


def parse_block(body):
    """Pairs of (command, {field: value}) in the order the block lists them."""
    runs = []
    command = None
    for raw_line in body:
        line = raw_line.strip()
        if line.startswith("run:"):
            if command is not None:
                runs.append((command, {}))
            command = line[len("run:") :].strip()
        elif line.startswith("then:") and command is not None:
            runs.append((command, dict_of(line[len("then:") :])))
            command = None
    if command is not None:
        runs.append((command, {}))
    return runs


def dict_of(text):
    """First occurrence wins, matching how compare_runs.sh reads a line."""
    values = {}
    for field, value in FIELD_RE.findall(text):
        values.setdefault(field, value)
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", type=Path, nargs="+")
    parser.add_argument("--binary", default="out/build/linux-debug/toy_quant")
    args = parser.parse_args()

    checked = 0
    failures = []
    for document in args.documents:
        text = document.read_text()
        for line_number, runs in check_blocks(text):
            for command, expected in runs:
                checked += 1
                completed = subprocess.run(
                    [args.binary] + shlex.split(command), capture_output=True, text=True
                )
                observed = dict_of(completed.stdout + completed.stderr)
                for field, want in expected.items():
                    have = observed.get(field)
                    if have != want:
                        failures.append(
                            f"{document}:{line_number}: {field} is {have!r}, "
                            f"the document says {want!r}"
                        )

    for failure in failures:
        print(f"FAIL {failure}")
    print(
        f"checked {checked} recorded run(s) in {len(args.documents)} document(s), "
        f"{len(failures)} mismatch(es)"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
