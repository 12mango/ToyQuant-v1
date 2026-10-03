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

What this tool does not do is re-measure. The numbers are copied from the documents, each case names where from, and
this is the second of two layers: this one checks that the quotients follow from the inputs, that the binding gate is
the one the arithmetic names, and that no optimistic bound was read as an opening. Re-deriving the *inputs* from the
archives is the other layer, and it needs the data.

Usage:
    python3 tools/viability.py
    python3 tools/viability.py --fixture tests/fixtures/verdicts.json
"""

import argparse
import json
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


def check(case):
    """The claims a recorded line makes, each of which the arithmetic has to support."""
    outcomes, binding = evaluate(case)
    problems = []
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


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, default=Path("tests/fixtures/verdicts.json"))
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
    return 1 if mismatched else 0


if __name__ == "__main__":
    sys.exit(main())
