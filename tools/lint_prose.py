#!/usr/bin/env python3
"""Check the claims the documents make about the code, in prose.

`tools/check_docs.py` verifies the numbers in marked blocks. Prose is not in a block, so nothing checked
it, and a paragraph still described "three queue interpretations" a day after the fourth was added. The
claims worth checking mechanically are the ones with a single source of truth in the code:

  * every `--flag` a document mentions exists in one of the binaries' parsers, and every flag the main
    binary accepts is documented;
  * every queue model is mentioned, and no document names a model that does not exist;
  * every path under `tools/` a document refers to exists;
  * the tree's file and line counts, the size table's entries and the test-binary count are counted again
    here rather than trusted.

What it does not check is the other half of a counted claim: a figure that describes a past run, a timing,
or a ratio between two numbers that were never measured against each other. The list above is the part with
a single source of truth that a program can read.

Usage:
    python3 tools/lint_prose.py docs/PERFORMANCE_HISTORY.md docs/ARCHITECTURE.md docs/USER_GUIDE.md ...
"""

import argparse
import re
import sys
from pathlib import Path

FLAG_LITERAL = re.compile(r'"(--[\w-]+)=?"')
MODEL_ENUM = re.compile(r"enum class QueueModel[^}]*\{(?P<body>[^}]*)\}", re.DOTALL)
MODEL_NAME_CASE = re.compile(r'case QueueModel::\w+:\s*\n\s*return "(\w+)"')
DOCUMENTED_FLAG = re.compile(r"`(--[\w-]+)")
TOOL_PATH = re.compile(r"\b(tools/[\w.]+\.(?:sh|py))\b")
SOURCE_FILE = re.compile(r"\.(?:h|cpp)$")
# The tree's size as the documents state it, and the two shorter ways the same claim is made. Each pattern
# is required to match: a checker that goes quiet because a sentence was reworded is worse than no checker,
# because it is still believed.
SIZE_BREAKDOWN = re.compile(r"(\d[\d,]*) files, ([\d,]+) lines: (.+)")
BREAKDOWN_ENTRY = re.compile(r"`([\w/]+)` ([\d,]+)")
SOURCE_LINE_CLAIM = re.compile(r"is ([\d,]+) lines of source")
TEST_BINARY_CLAIM = re.compile(r"(\d+) (?:unit-)?test binaries")
TEST_TARGET = re.compile(r"^\s*add_toy_test\((\w+)\)", re.MULTILINE)


def usage_text(source):
    """The body of `print_usage`, which is the binary's own help.

    Empty when it cannot be found, and the caller reports that rather than skipping: a check that goes quiet
    when a function is renamed is still believed, which is worse than no check.
    """
    lines = source.splitlines()
    starts = [index for index, line in enumerate(lines) if line.startswith("void print_usage")]
    ends = [index for index, line in enumerate(lines) if line.startswith("bool parse_config")]
    if not starts or not ends:
        return ""
    return "\n".join(lines[starts[0]:ends[0]])


def counted_source(root):
    """Files and lines per directory, with `lines/legacy` counted on its own.

    Lines are newlines, which is what `wc -l` counts, because those are the numbers the documents already
    carry. A checker that disagreed with how the figure was measured would be a checker nobody trusts.
    """
    counts = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or not SOURCE_FILE.search(path.name):
            continue
        parts = path.relative_to(root).parts
        if "legacy" in parts:
            key = "legacy"
        elif len(parts) == 1:
            # `src/main.cpp` has no directory of its own, and the size table used to leave it out while the
            # total counted it, which is why the table's entries did not add up to its own total.
            key = "main"
        else:
            key = parts[0]
        entry = counts.setdefault(key, [0, 0])
        entry[0] += 1
        entry[1] += path.read_text().count("\n")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", type=Path, nargs="+")
    parser.add_argument("--config", type=Path, default=Path("src/app/app_config.cpp"),
                        help="the main binary's parser, whose flags the documents have to cover")
    # The benchmark driver has a parser of its own, and a documented `--timeline` failed this check the
    # moment it grew one. Documents may mention this parser's flags; they do not have to cover all of them,
    # because the driver's own `--help` is the list and `docs/USER_GUIDE.md` points at it -- a paragraph per
    # tuning knob would be a second list to keep in step.
    parser.add_argument("--driver-config", type=Path,
                        default=Path("src/app/strategy_benchmark_main.cpp"))
    # The queue models live beside the enum, not in the engine that switches on them.
    parser.add_argument("--types", type=Path, default=Path("src/common/types.h"))
    # The counted claims: the tree the size figures describe, and the build that says how many test binaries
    # the gate runs.
    parser.add_argument("--source", type=Path, default=Path("src"))
    parser.add_argument("--cmake", type=Path, default=Path("CMakeLists.txt"))
    args = parser.parse_args()

    config = args.config.read_text()
    types = args.types.read_text()
    # Every flag literal in the parser, however it is matched: a prefix constant and a plain comparison
    # both end up as the same string, and looking for the constants alone missed the plain ones.
    declared = {flag for flag in FLAG_LITERAL.findall(config)}
    mentioned = declared | {flag for flag in FLAG_LITERAL.findall(args.driver_config.read_text())}
    models = {name.lower() for name in MODEL_NAME_CASE.findall(types)}
    if not declared or not models:
        print("FAIL could not read the flags or the queue models out of the source")
        return 1

    text = "\n".join(path.read_text() for path in args.documents)
    problems = []

    for flag in sorted(declared - {"--help"}):
        if f"`{flag}" not in text:
            problems.append(f"{flag} is accepted by the code and not mentioned in any document")
    for flag in sorted({m for m in DOCUMENTED_FLAG.findall(text)} - mentioned):
        # Flags of the Python tools and of the strategies are documented and are not this parser's, and
        # --help is the one flag every binary has without saying so.
        if flag not in {"--order-size", "--inventory-limit", "--queue-model", "--min-trades-per-second",
                        "--download", "--input", "--pretty-px", "--help"}:
            problems.append(f"{flag} is mentioned in a document and is not a flag the binary accepts")
    for model in sorted(models):
        if model not in text.lower():
            problems.append(f"queue model {model} exists and is not mentioned in any document")
    # Every flag the parser takes is named in its own help text. The check above says the documents cover
    # the flags; it says nothing about what a user gets from `--help`, and eleven flags were missing from it
    # -- `--trace-order`, the two risk limits, the two latency knobs and the rest of what the L2 path takes
    # -- every one of them written up in a document and none of them discoverable from the binary.
    usage = usage_text(config)
    if not usage:
        problems.append(f"could not find the usage text in {args.config}")
    else:
        for flag in sorted(declared):
            if flag.rstrip("=") not in usage:
                problems.append(f"{flag} is accepted by the binary and missing from its own --help text")
    for name in sorted(set(TOOL_PATH.findall(text))):
        if not Path(name).exists():
            problems.append(f"{name} is referenced by a document and does not exist")

    # The counts. They are hand-copied into prose, nothing else reads them, and one drifted for a whole
    # commit before anyone noticed: the CLI change added ninety-eight lines to `app` while the size table
    # still said 4120. This is the part of the documents that says how big the repository is, so it is
    # measured here instead of believed.
    counts = counted_source(args.source)
    if not counts:
        problems.append(f"no source files found under {args.source}")
    total_files = sum(entry[0] for entry in counts.values())
    total_lines = sum(entry[1] for entry in counts.values())

    size = SIZE_BREAKDOWN.search(text)
    if size is None:
        problems.append("no document states the size of the tree as '<files> files, <lines> lines: ...'")
    else:
        if int(size.group(1).replace(",", "")) != total_files:
            problems.append(
                f"a document states {size.group(1)} source files and the tree has {total_files}")
        if int(size.group(2).replace(",", "")) != total_lines:
            problems.append(
                f"a document states {size.group(2)} source lines and the tree has {total_lines}")
        entries = {name: int(value.replace(",", ""))
                   for name, value in BREAKDOWN_ENTRY.findall(size.group(3))}
        for name, stated in sorted(entries.items()):
            measured = counts.get(name)
            if measured is None:
                problems.append(f"the size table names `{name}` and the tree has no such directory")
            elif measured[1] != stated:
                problems.append(f"`{name}` has {measured[1]} lines and the size table states {stated}")
        for name in sorted(set(counts) - set(entries)):
            problems.append(f"the tree has `{name}` and the size table leaves it out")
        if sum(entries.values()) != total_lines:
            problems.append(f"the size table's entries add up to {sum(entries.values())} and the total "
                            f"states {total_lines}")

    for stated in SOURCE_LINE_CLAIM.findall(text):
        if int(stated.replace(",", "")) != total_lines:
            problems.append(f"a document states {stated} source lines and the tree has {total_lines}")
    test_targets = TEST_TARGET.findall(args.cmake.read_text())
    for stated in TEST_BINARY_CLAIM.findall(text):
        if int(stated) != len(test_targets):
            problems.append(f"a document states {stated} test binaries and the build registers "
                            f"{len(test_targets)}")

    for problem in problems:
        print(f"FAIL {problem}")
    print(
        f"checked {len(mentioned)} flags, {len(models)} queue models and {total_files} source files "
        f"({total_lines} lines) against {len(args.documents)} document(s), {len(problems)} problem(s)"
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
