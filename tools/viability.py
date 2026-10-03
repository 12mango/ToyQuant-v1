#!/usr/bin/env python3
"""Re-derive the eight closed lines from the numbers the documents record, and refuse a verdict the arithmetic does not support.

The retrospective names the criteria itself, as the same three in a fixed order -- **the fee, then the position's own
move, then the number of actions a window contains** -- with the binding one moving down that list on each new venue.
This tool makes that sentence executable, over the eight lines already closed:

    fee       the capture a fill can collect against the fee the schedule charges for one
    position  the capture a window collects against the standard deviation of the position it leaves behind
    actions   the prints a fill can absorb, R_max, against the 71 the equity fill model assumed

A quote is closed at the first gate it fails, so the verdict is not a parameter and not an opinion: it is the name of
the gate that fired.

**The bound is the part that matters, and it is why a verdict can be reached without the sample a test statistician
would ask for.** To close a door it is enough to show that the *most favourable* case still loses: the widest
spread, the smallest position move, the best window. That is a statement about a bound, and one window settles it,
because a bound that fails in the best case fails everywhere. To *open* a door the same argument cannot be run, since
opening one needs the opposite bound -- the least favourable case still wins -- and that is what costs the hundreds of
windows the MU line stopped short of. So every case here carries `bound: optimistic`, and this tool enforces the
direction rather than trusting it:

    optimistic + closed   legal, and the only shape a recorded line may take
    optimistic + open     refused, because the best case passing proves nothing about the rest

**A gate that was not measured is not a gate that held.** The engine's own summary carried four quantities that were
never computed and nothing noticed, and a field read but never written looks exactly like a field whose value is
zero. The same failure is available here, so every gate reports one of three states, and they are printed:

    fired          the recorded numbers put it below the bar, and the quotient comes with it
    held           the numbers clear the bar
    not measured   no numbers are recorded for it, which is *not* a pass and never determines a binding

What this tool does not do is re-measure, unless it is asked to. The numbers are copied from the documents, each case
names where from, and the checkable half is two layers. Without `--derive` it is the first: the quotients follow from
the inputs, the binding gate is the one the arithmetic names, and no optimistic bound was read as an opening.
`--derive` is the second and it does re-measure, by running the tools that produced two of the inputs -- the low
turnover rule's median, and the option series' R_max -- against the archives on disk, and comparing what comes back
with what the fixture recorded, so a recorded figure and a measured one have to stay the same number. It needs the
data and takes minutes, so it is not in the gate.

Usage:
    python3 tools/viability.py
    python3 tools/viability.py --fixture tests/fixtures/verdicts.json
    python3 tools/viability.py --derive       # layer two: re-measures, needs the archives, not in the gate
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# The fixed order the retrospective names, and the shape each line had. A quote is closed at the first gate it fails;
# the other three structures settle their question at one gate, because a carry is not a quote and does not requote.
GATE_ORDER = {
    "quote": ("fee", "position", "actions"),
    "carry": ("payback",),
    "hold": ("sign",),
    "restricted": ("restriction",),
}

# A fill has to collect at least the fee it paid, and a window has to collect at least the move it leaves behind.
# Both are the natural bar rather than a chosen one: below it the cost or the position grows faster than the capture.
BAR = 1.0
# The prints per fill the equity screen's fill model used, quoted in `tools/venue_ratio.py` and here.
REFERENCE_PRINTS = 71.0


def fee_gate(case):
    """What a fill collects against what one action costs."""
    inputs = case["inputs"]
    if "captured_ticks" in inputs and "fee_ticks" in inputs:
        quotient = inputs["captured_ticks"] / inputs["fee_ticks"]
        return ("fired" if quotient < BAR else "held"), f"{quotient:.3f}", "derived"
    # The fifteen day line passed this axis first, and the documents record the pass without the two ticks it was
    # computed from, so the gate holds on the recorded claim and says so rather than presenting it as arithmetic.
    if inputs.get("fee_axis") == "passed":
        return "held", "recorded pass", "recorded"
    return "not measured", None, "none"


def position_gate(case):
    """What a window collects against the standard deviation of the position it leaves behind."""
    inputs = case["inputs"]
    if "capture_usd" in inputs and "sigma_usd" in inputs:
        quotient = inputs["capture_usd"] / inputs["sigma_usd"]
        return ("fired" if quotient < BAR else "held"), f"{quotient:.3f}", "derived"
    return "not measured", None, "none"


def actions_gate(case):
    """The prints a fill can absorb against the reference the fill model assumed."""
    inputs = case["inputs"]
    if "r_max" in inputs:
        reference = inputs.get("reference_prints", REFERENCE_PRINTS)
        return ("fired" if inputs["r_max"] < reference else "held"), \
            f"{inputs['r_max']:g} of {reference:g} allowed", "derived"
    return "not measured", None, "none"


def payback_gate(case):
    """Whether any month of the tested span cleared its own round trip."""
    inputs = case["inputs"]
    if "months_clearing" in inputs:
        state = "fired" if inputs["months_clearing"] < 1 else "held"
        return state, f"{inputs['months_clearing']} of {inputs['months_tested']} months clear", "derived"
    return "not measured", None, "none"


def sign_gate(case):
    """Whether the rule's own sign survives the windows it was run on."""
    inputs = case["inputs"]
    if "median_net_cents" in inputs:
        state = "fired" if inputs["median_net_cents"] <= 0.0 else "held"
        return state, f"median {inputs['median_net_cents']:.2f} cents, " \
            f"{inputs.get('positive', '?')} of {inputs.get('windows', '?')} windows positive", "derived"
    return "not measured", None, "none"


def restriction_gate(case):
    """Whether the series sits inside an expiry band the project excluded before measuring."""
    inputs = case["inputs"]
    if "dte" in inputs and "excluded_dte_max" in inputs:
        state = "fired" if inputs["dte"] < inputs["excluded_dte_max"] else "held"
        return state, f"dte {inputs['dte']} against a {inputs['excluded_dte_max']} day floor", "derived"
    return "not measured", None, "none"


GATES = {"fee": fee_gate, "position": position_gate, "actions": actions_gate,
         "payback": payback_gate, "sign": sign_gate, "restriction": restriction_gate}


def evaluate(case):
    """Every gate of the line's shape in the fixed order, and the first one that fired as the binding."""
    outcomes = []
    for name in GATE_ORDER[case["structure"]]:
        state, detail, source = GATES[name](case)
        outcomes.append({"gate": name, "state": state, "detail": detail, "source": source})
    binding = next((outcome["gate"] for outcome in outcomes if outcome["state"] == "fired"), None)
    return outcomes, binding


def separation_of(case):
    """The windows a two sigma separation needs: (2 sigma / capture)^2, from the same two inputs the gate reads."""
    spec = case.get("separation")
    if not spec:
        return None
    return (2.0 * case["inputs"][spec["sigma"]] / case["inputs"][spec["capture"]]) ** 2


def check(case):
    """The claims a recorded line makes, each of which the arithmetic has to support."""
    outcomes, binding = evaluate(case)
    problems = []
    spec = case.get("separation")
    if spec:
        needed = separation_of(case)
        if abs(needed - spec["windows"]) > 1.0:
            problems.append(f"a two sigma separation needs {needed:.0f} windows, and the line records "
                            f"{spec['windows']}")
        else:
            outcomes.append({"gate": "separation", "state": "held", "detail":
                             f"{needed:.0f} windows at two sigma", "source": "derived"})
    if case["bound"] != "optimistic":
        problems.append(f"bound is {case['bound']}, and only an optimistic bound may close a line here")
    if case["verdict"] == "closed" and binding is None:
        problems.append("recorded closed, and no gate fired, so the arithmetic does not close it")
    if case["verdict"] == "open" and binding is not None:
        problems.append(f"recorded open, and the {binding} gate fired")
    if case["bound"] == "optimistic" and case["verdict"] == "open":
        problems.append("an optimistic bound cannot open a line: the best case passing proves nothing")
    if binding != case["binding"]:
        problems.append(f"the first gate to fire is {binding or 'none'}, recorded as {case['binding']}")
    return outcomes, binding, problems


def render(outcomes):
    """Each gate's state with the number or the span behind it, so a verdict shows what it rested on."""
    parts = []
    for outcome in outcomes:
        detail = f" {outcome['detail']}" if outcome["detail"] else ""
        parts.append(f"{outcome['gate']}={outcome['state'].replace(' ', '-')}{detail}")
    return " ".join(parts)


# Layer two: the inputs themselves, re-derived from the data and the tools that produced them. This is deliberately
# not part of tier 1. It needs the archives and it takes minutes, where the gate runs on every edit. What it buys is
# that the fixture's numbers stop being copies: a derivation that disagrees with the fixture is a mismatch and not a
# footnote, so the recorded figure and the measured one have to stay the same number.
SOURCES = {
    # The eight MU windows the low turnover rule was run on, which is the table its median came from.
    "lowturnover": {
        "cmd": ["python3", "tools/lowturnover_test.py", "--glob",
                "data/v2/equities/xnas_MU_depth_*.csv.gz"],
        "needs": "data/v2/equities",
        "read": r"median net a share a round trip ([+-]?\d+\.?\d*) cents",
    },
    # The option series bought for the fifteen day line, through the same quotient `tools/venue_ratio.py` reports:
    # R_max = prints x (half spread - cost a unit) / sigma, read against the 71 reference. The tool prints `highest:`
    # only when something clears that reference, and here nothing does -- that is the finding -- so the number comes
    # off the first row of the table instead, which is the highest because the table is sorted by best R_max, and
    # from its clip of one column, which is the one the fixture records.
    "venue_ratio_options": {
        "cmd": ["python3", "tools/venue_ratio.py", "--input", "data/v2/options/*.csv",
                "--multiplier", "100", "--fee-per-unit", "0.65", "--min-commission", "1.00",
                "--clip", "1,2", "--label", "options"],
        "needs": "data/v2/options",
        "read": r"\n\| [^|\n]+ \|\s*\d+ \|\s*[\d.]+ \|\s*[\d.]+ \|\s*[\d.]+ \|\s*(\d+) \|",
    },
    # The same quotient over the sixty equity windows. It feeds no gate: the documents record the one lot ratio as a
    # necessary condition and not the engine's position term, which is a different quantity, so it is reported here
    # as a reading and never compared against a gate input.
    "venue_ratio_equities": {
        "cmd": ["python3", "tools/venue_ratio.py", "--input", "data/v2/screen/*.csv",
                "--clip", "1,100", "--label", "equities"],
        "needs": "data/v2/screen",
        "read": r"\n\| [^|\n]+ \|\s*\d+ \|\s*[\d.]+ \|\s*[\d.]+ \|\s*[\d.]+ \|\s*(\d+) \|",
    },
}


def derive(source, cache):
    """Run one tool that produced an input, and take back the number. Cached, because a source serves one line."""
    if source in cache:
        return cache[source]
    spec = SOURCES[source]
    if not list(Path(spec["needs"]).glob("*")):
        cache[source] = (None, f"{spec['needs']} holds no file")
        return cache[source]
    try:
        done = subprocess.run(spec["cmd"], capture_output=True, text=True, timeout=3600)
    except (OSError, subprocess.SubprocessError) as problem:
        cache[source] = (None, f"{spec['cmd'][1]} would not run ({problem})")
        return cache[source]
    found = re.search(spec["read"], done.stdout)
    if done.returncode != 0 or not found:
        cache[source] = (None, f"{spec['cmd'][1]} exited {done.returncode} without the line it is read for")
        return cache[source]
    cache[source] = (float(found.group(1)), None)
    return cache[source]


def derive_all(cases):
    """Every derivation a case declares, against the input it is supposed to reproduce."""
    cache = {}
    mismatched = 0
    for case in cases:
        declaration = case.get("derive")
        if not declaration:
            continue
        measured, problem = derive(declaration["source"], cache)
        recorded = case["inputs"][declaration["input"]]
        if problem:
            # A derivation that could not run leaves the number where it was, which is recorded rather than
            # measured, and it says so rather than printing the recorded value back as if it had been reproduced.
            print(f"SKIP -- {case['line']}: {declaration['input']} stays recorded at {recorded} -- {problem}")
            continue
        agree = abs(measured - recorded) <= 0.5
        mismatched += 0 if agree else 1
        print(f"{'OK  ' if agree else 'FAIL'} derive {case['line']:<34} {declaration['input']} "
              f"recorded={recorded:g} measured={measured:g}")
    print(f"derivations from {len(cache)} source(s), {mismatched} mismatch")
    return mismatched


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, default=Path("tests/fixtures/verdicts.json"))
    parser.add_argument("--derive", action="store_true",
                        help="also re-derive the inputs from the data on disk; needs the archives and takes "
                             "minutes, so it is not part of the tier 1 gate")
    args = parser.parse_args()

    cases = json.loads(args.fixture.read_text())["cases"]
    mismatched = 0
    tally = {"derived": 0, "recorded": 0, "none": 0}
    progression = []
    for case in cases:
        outcomes, binding, problems = check(case)
        for outcome in outcomes:
            tally[outcome["source"]] += 1
        progression.append(binding)
        if problems:
            mismatched += 1
            print(f"FAIL {case['line']} ({case['source']})")
            for problem in problems:
                print(f"  {problem}")
            print(f"  {render(outcomes)}")
        else:
            print(f"OK   {case['line']:<34} bind={binding or 'none':<11} {render(outcomes)}")
        for note in case.get("also", []):
            # The numbers that closed the same line from a second direction, or that were set aside on purpose.
            # They are printed and not gated: a second kill is not the binding one, and an exclusion is not a
            # quotient at all.
            print(f"     also: {note}")
    print(f"checked {len(cases)} recorded verdicts, {mismatched} mismatch")
    print(f"gates derived={tally['derived']} recorded={tally['recorded']} not measured={tally['none']}")
    print("binding gate, in the order the lines were closed: " + " ".join(one or "none" for one in progression))
    if args.derive:
        print()
        mismatched += derive_all(cases)
    return 1 if mismatched else 0


if __name__ == "__main__":
    sys.exit(main())
