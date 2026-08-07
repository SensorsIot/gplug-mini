#!/usr/bin/env python3
"""Write a pytest run's results into testing/test-plan.yaml.

    tools/record-results.py <pytest-log> [--commit <sha>] [--dry-run]

`status`, `reason`, `commit` and `when` belong to the runner and to nothing
else. Hand-editing them is how a plan acquires results nobody produced — and on
2026-08-05 it is also how this file was corrupted, by an unquoted colon in a
`reason`. So the runner writes them, through here, and the YAML is re-parsed
before it is saved.

Node ids carry their case id by convention (`test_ts108_...` -> TS-108), which
is what makes this possible without a second mapping to keep in step. A node
whose id names no case in the plan is reported rather than silently dropped.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import time

try:
    import yaml
except ImportError:
    sys.exit("needs pyyaml")

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLAN = ROOT / "testing/test-plan.yaml"

# pytest's own vocabulary -> the plan's. XFAIL is deliberately NOT `successful`:
# an expected failure is a declared gap, and recording it as a pass would let a
# known-unverified contract count towards a release gate.
OUTCOME = {
    "PASSED": "successful",
    "FAILED": "failed",
    "ERROR": "not done",
    "SKIPPED": "not done",
    "XFAIL": "not done",
}
# Which tiers a log may write. A bench run must never satisfy a field case, and
# the node-name convention below cannot tell the difference on its own: a bench
# node called `test_ts021_...` was once recorded against a FIELD case, reporting
# a real-meter comparison as passing on evidence that never involved a meter.
TIERS_FOR = {"testing/bench": {"bench"}}

RESULT_LINE = re.compile(
    # The bracketed part of a parametrised node can contain spaces, so the node
    # is not simply non-whitespace: `test_ts038_...[broker-no broker]`. Matching
    # \S+ alone silently dropped every parametrised case from the record.
    r"^(?P<path>testing/bench/\S+?)::(?P<node>\S+?(?:\[[^\]]*\])?)\s+"
    r"(?P<outcome>PASSED|FAILED|ERROR|SKIPPED|XFAIL|XPASS)\b"
)
CASE_IN_NODE = re.compile(r"_(ts|wf)(\d{3})", re.I)


def parse(log_path):
    """(case_id, outcome, node) for every result line in the log."""
    out = []
    for line in pathlib.Path(log_path).read_text().splitlines():
        m = RESULT_LINE.match(line.strip())
        if not m:
            continue
        found = CASE_IN_NODE.search(m.group("node"))
        if not found:
            out.append((None, m.group("outcome"), m.group("node")))
            continue
        prefix = found.group(1).upper()
        out.append((f"{prefix}-{found.group(2)}", m.group("outcome"), m.group("node")))
    return out


def status_rank(outcome):
    """How strong a claim an outcome makes. A failure outranks a pass."""
    return {"FAILED": 2, "ERROR": 2}.get(outcome, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--commit", default=None)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    commit = a.commit or subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
        capture_output=True, text=True).stdout.strip()
    when = time.strftime("%Y-%m-%d")

    raw = PLAN.read_text()
    plan = yaml.safe_load(raw)
    by_id = {t["id"]: t for t in plan["test_cases"]}

    results = parse(a.log)
    if not results:
        sys.exit(f"no result lines found in {a.log}")

    changes, unmatched, refused = [], [], []
    seen_failed = {cid for cid, outcome, _ in results if outcome in ("FAILED", "ERROR")}
    for case_id, outcome, node in results:
        if case_id is None or case_id not in by_id:
            unmatched.append((node, outcome))
            continue
        case = by_id[case_id]
        # A parametrised case is one case. pytest reports one line per
        # parametrisation, and last-wins would record TS-038 as passing because
        # its `ssid` variant happened to run after its `pass` variant failed.
        # Each parametrisation is a separate obligation, so any failure is the
        # case's result.
        if case_id in seen_failed and status_rank(outcome) < 2:
            continue
        allowed = TIERS_FOR.get("testing/bench", set())
        if case.get("tier") not in allowed:
            refused.append((node, case_id, case.get("tier")))
            continue
        status = OUTCOME.get(outcome, "not done")
        before = case.get("status")
        case["status"] = status
        case["commit"] = commit if status in ("successful", "failed") else None
        case["when"] = when if status in ("successful", "failed") else None
        if status == "successful":
            # A reason on a pass is noise; the commit and date say everything.
            case["reason"] = None
        elif outcome == "XFAIL":
            case["reason"] = ("declared xfail - the contract is real but this rig "
                              "cannot decide it; see the capability note")
        elif outcome == "ERROR":
            case["reason"] = ("errored in setup, so nothing was learned about the "
                              "requirement - fix the precondition and re-run")
        changes.append((case_id, before, status))

    for node, outcome in unmatched:
        print(f"  no plan entry for {node} ({outcome})")
    for node, case_id, tier in refused:
        print(f"  REFUSED {node} -> {case_id}: a bench run cannot decide a "
              f"{tier}-tier case")

    print(f"\n{len(changes)} case(s) updated at {commit}, {when}")
    for cid, before, after in sorted(changes):
        mark = "" if before == after else f"   ({before} -> {after})"
        print(f"  {cid:8} {after}{mark}")

    if a.dry_run:
        print("\ndry run - nothing written")
        return

    text = yaml.dump(plan, sort_keys=False, width=120, allow_unicode=True)
    yaml.safe_load(text)          # refuse to save something that will not re-parse
    PLAN.write_text(text)
    print(f"\nwritten to {PLAN}")


if __name__ == "__main__":
    main()
