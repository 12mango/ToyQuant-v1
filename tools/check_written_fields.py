#!/usr/bin/env python3
"""Check that every field of a metric struct is written somewhere under src/.

The failure this exists for is specific and it happened here: one line of the run summary carried four
quantities that were never computed, and nothing noticed. A field that is read but never written looks
exactly like a field whose value happens to be zero, so a reader cannot tell "no fills" from "nobody
counted the fills", and neither can a test that asserts the value it already has.

Every field therefore has to appear on the left of an assignment, an increment, or a compound
assignment somewhere under src/. Fields read across a struct boundary are fine, because the writer
names them with the same identifier, which is what this matches on.

Usage:
    python3 tools/check_written_fields.py [--src src]
"""

import argparse
import re
import sys
from pathlib import Path

STRUCTS = ("ExecutionQualityMetrics", "StrategyMetrics", "PortfolioMetrics", "RunSummary")
FIELD = re.compile(
    r"^\s+(?:const\s+)?(?:std::string|std::size_t|double|float|int64_t|uint64_t|int|bool|size_t)\s+(\w+)\{",
    re.M,
)
# Written means: incremented, decremented, or assigned. `==` is deliberately not an assignment, and it
# is the reason this pattern spells the operators out instead of looking for a bare equals sign.
WRITTEN = r"(?:\+\+|--|\+=|-=|\*=|/=|=(?!=))"


def fields_of(name, text):
    try:
        start = text.index(f"struct {name}")
    except ValueError:
        return None
    # The closing brace has to be the one at the start of a line: a field default like `{0.0};` also
    # contains the two characters `};`, and matching that truncated the struct to its first field.
    end = text.index("\n};", start)
    return [match.group(1) for match in FIELD.finditer(text[start:end])]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, default=Path("src"))
    args = parser.parse_args()

    files = sorted(list(args.src.rglob("*.h")) + list(args.src.rglob("*.cpp")))
    contents = {path: path.read_text() for path in files}
    combined = "\n".join(contents.values())

    missing = []
    checked = 0
    for name in STRUCTS:
        declared = None
        for text in contents.values():
            found = fields_of(name, text)
            if found is not None:
                declared = found
                break
        if declared is None:
            print(f"FAIL {name} was not found under {args.src}")
            return 1
        for field in declared:
            checked += 1
            if not re.search(rf"\b{field}\b\s*{WRITTEN}", combined):
                missing.append(f"{name}.{field}")

    for entry in missing:
        print(f"FAIL {entry} is declared and never written under {args.src}")
    print(f"checked {checked} metric fields in {len(STRUCTS)} structs, {len(missing)} unwritten")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
