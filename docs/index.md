# ToyQuant Documentation

Start from the reason you are reading. The last column says who keeps each document honest, because in this
project that is a mechanical answer: most of the claims here are re-derived by the build.

## Pick the path

| If you want to... | Read | Then |
|---|---|---|
| evaluate the design | [Design Review](DESIGN_REVIEW.md) | [Architecture](ARCHITECTURE.md) for what exists |
| understand market making | [Learning Path](LEARNING.md) | [Design Review](DESIGN_REVIEW.md) section 4 for the evidence |
| run it | [User Guide](USER_GUIDE.md) | [Data Files](../data/README.md) for the input contracts |
| know what it costs | [Performance](PERFORMANCE.md) | [Low-Latency Design](LATENCY_DESIGN.md) for why each change is faster |
| see how a number was obtained | [Performance History](PERFORMANCE_HISTORY.md) | the section named in whichever document quoted it |

**If you only read one document, read the [Design Review](DESIGN_REVIEW.md).** It carries the constraints the
project holds itself to, the decisions and the rejected alternatives behind each component, the measured
headline results, an explicit list of what is wrong with the demo, and the three lengths of the same story
for talking about it out loud.

## Every document

| Document | What it is for | Not the place for | Kept honest by |
|---|---|---|---|
| [Design Review](DESIGN_REVIEW.md) | Why the simulator is built this way, the C++ decisions with code anchors and measured effects, the headline results, the limitations, and the interview versions | the module reference (`ARCHITECTURE`), or the raw measurement records (`PERFORMANCE_HISTORY`) | its prose is checked for flags, queue models and tool paths by `tools/lint_prose.py` |
| [Architecture](ARCHITECTURE.md) | What each module is, the data contracts between them, and the matching rules | the rationale and trade-offs (`DESIGN_REVIEW`) | marked blocks re-run by `tools/check_docs.py`; prose linted |
| [Learning Path](LEARNING.md) | The market-making mechanisms the simulator exists to show, and a field reference | engineering decisions, or how to run anything | prose linted |
| [User Guide](USER_GUIDE.md) | Every mode, every flag, and the failure modes that waste an afternoon | why anything is designed the way it is (`DESIGN_REVIEW`) | prose linted, so a flag it forgets or invents fails the build |
| [Performance](PERFORMANCE.md) | Workloads, how to measure on this host, the current stage breakdown, and the ledger of every optimization attempted | the mechanism behind each accepted change (`LATENCY_DESIGN`) | the ledger's claims are paired measurements from `tools/measure_l2_baseline.py` |
| [Low-Latency Design](LATENCY_DESIGN.md) | Why each low-latency change is faster: problem, mechanism, and the designs that were measured and rejected | the numbers themselves (`PERFORMANCE`) | prose linted |
| [Performance History](PERFORMANCE_HISTORY.md) | Measurements in the order they were taken, kept as a lab notebook, superseded ones included | the current set of conclusions | marked blocks re-run by `tools/check_docs.py` |
| [Data Files](../data/README.md) | Tick CSV format, runtime outputs, typical workflow | - | - |

## What the project covers, and what it deliberately does not

The loop is feed adapters, an order book, a strategy, a matching engine with an explicit queue model,
execution reports, and a portfolio, in C++20 with CMake and a Python 3 toolchain for the measurement tools.

It is not a production exchange stack, a connectivity layer, or a live-trading system. There is no market
impact, no queue-position feedback from the strategy's own resting size, no persistence or recovery, and no
risk gateway. Those exclusions are what keep the whole loop small enough to read in one sitting, and they are
listed as limitations rather than hidden as gaps in the [Design Review](DESIGN_REVIEW.md).
