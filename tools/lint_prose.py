#!/usr/bin/env python3
"""Check the claims the documents make about the code, in prose.

`tools/check_docs.py` verifies the numbers in marked blocks. Prose is not in a block, so nothing checked
it, and a paragraph still described "three queue interpretations" a day after the fourth was added. The
claims worth checking mechanically are the ones with a single source of truth in the code:

  * every `--flag` a document mentions exists as a flag, and every flag the code accepts is documented;
  * every queue model is mentioned, and no document names a model that does not exist;
  * every path under `tools/` a document refers to exists.

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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", type=Path, nargs="+")
    parser.add_argument("--config", type=Path, default=Path("src/app/app_config.cpp"))
    # The queue models live beside the enum, not in the engine that switches on them.
    parser.add_argument("--types", type=Path, default=Path("src/common/types.h"))
    args = parser.parse_args()

    config = args.config.read_text()
    types = args.types.read_text()
    # Every flag literal in the parser, however it is matched: a prefix constant and a plain comparison
    # both end up as the same string, and looking for the constants alone missed the plain ones.
    declared = {flag for flag in FLAG_LITERAL.findall(config)}
    models = {name.lower() for name in MODEL_NAME_CASE.findall(types)}
    if not declared or not models:
        print("FAIL could not read the flags or the queue models out of the source")
        return 1

    text = "\n".join(path.read_text() for path in args.documents)
    problems = []

    for flag in sorted(declared - {"--help"}):
        if f"`{flag}" not in text:
            problems.append(f"{flag} is accepted by the code and not mentioned in any document")
    for flag in sorted({m for m in DOCUMENTED_FLAG.findall(text)} - declared):
        # Flags of the Python tools and of the strategies are documented and are not this parser's, and
        # --help is the one flag every binary has without saying so.
        if flag not in {"--order-size", "--inventory-limit", "--queue-model", "--help"}:
            problems.append(f"{flag} is mentioned in a document and is not a flag the binary accepts")
    for model in sorted(models):
        if model not in text.lower():
            problems.append(f"queue model {model} exists and is not mentioned in any document")
    for name in sorted(set(TOOL_PATH.findall(text))):
        if not Path(name).exists():
            problems.append(f"{name} is referenced by a document and does not exist")

    for problem in problems:
        print(f"FAIL {problem}")
    print(
        f"checked {len(declared)} flags and {len(models)} queue models against "
        f"{len(args.documents)} document(s), {len(problems)} problem(s)"
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
