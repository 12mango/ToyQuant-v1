#!/usr/bin/env python3
"""Check the layer boundary the CMakeLists states, so that it is enforced rather than trusted.

The engine's core is what a reader learns it from: `common`, then `orderbook`, `strategy`, `exchange` and
`accounting`, then `app`. A venue reader, a legacy header or the carry ledger inside that path turns a five thousand
line lesson into a fifteen thousand line one, and the coupling is easy to introduce by accident, because every include
root is shared. This tool makes it a build failure instead.

The rule is about files, not about a list, so it holds for a source added tomorrow: nothing in the core directories may
include a header that belongs to a line.

Usage:
    python3 tools/check_layering.py            # exit 1 and print every offending include
"""

import re
import sys
from pathlib import Path

CORE_DIRS = ["src/common", "src/orderbook", "src/strategy", "src/exchange", "src/accounting", "src/utils"]
# The core's own positive list: what `app/pipeline.cpp` and the L2 book are allowed to be part of.
CORE_EXTRA = ["src/app/pipeline.cpp", "src/app/run_summary.h", "src/orderbook/l2_orderbook.cpp"]
# Three files are lines that happen to live in a core directory: the carry ledger, the paper broker and the
# reconciliation that reads its statement. They are excluded rather than moved because moving them means touching
# every include that names them, and this tool exists to keep an accidental coupling out rather than to reorganise a
# directory. A `lines/` tree is the structural fix if the exclusion ever grows.
EXCLUDE = ["src/accounting/carry_ledger.h", "src/accounting/carry_ledger.cpp",
           "src/accounting/reconciliation.h", "src/accounting/reconciliation.cpp",
           "src/exchange/paper_broker.h", "src/exchange/paper_broker.cpp"]
# Headers that belong to a feed family, to the frozen legacy line, or to the carry ledger.
FORBIDDEN = [
    "legacy/",
    "market/binance_readers",
    "market/deribit_readers",
    "market/okx_readers",
    "market/market_data_adapter",
    "market/funding_rate.h",
    "market/csv_feed.h",
    "market/udp_feed.h",
    "market/market_event.h",
    "accounting/carry_ledger",
    "exchange/paper_broker",
]
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def core_files():
    files = []
    for path in CORE_EXTRA:
        files.append(Path(path))
    for directory in CORE_DIRS:
        for suffix in ("*.h", "*.cpp"):
            files.extend(sorted(Path(directory).glob(suffix)))
    return [path for path in files if path.is_file() and str(path) not in EXCLUDE]


def main():
    problems = []
    checked = 0
    for path in core_files():
        checked += 1
        for number, line in enumerate(path.read_text().splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            included = match.group(1)
            for forbidden in FORBIDDEN:
                if included.startswith(forbidden):
                    problems.append(f"{path}:{number}: {included} belongs to a line, not to the core")
    print(f"[LAYERS] checked {checked} core files against {len(FORBIDDEN)} forbidden include prefixes")
    if problems:
        for problem in problems:
            print(f"[LAYERS] FAIL {problem}")
        return 1
    print("[LAYERS] OK: no core file includes a venue, a legacy header or the carry ledger")
    return 0


if __name__ == "__main__":
    sys.exit(main())
