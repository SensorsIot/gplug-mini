#!/usr/bin/env python3
"""Render testing/test-plan.yaml into the report.

Nothing here is authored. Requirements come from the FSD, tests from the plan,
results from whatever ran them. The report is a view, so it cannot disagree with
its sources - which is the whole reason coverage is not a hand-kept column.

    tools/report.py            group by requirement
    tools/report.py --tier     group by tier
    tools/report.py --gaps     only what is not verified
    tools/report.py --gates    release-gate readiness

Atomic cases and workflows are never summed together. They answer different
questions - "which specified behaviour failed?" and "does the product do its
job?" - and a single passed/total hides the case where every component passes
and the journey between them does not.
"""
import argparse, collections, pathlib, re, sys

try:
    import yaml
except ImportError:
    sys.exit("needs pyyaml:  python3 -m venv .venv && .venv/bin/pip install pyyaml")

ROOT = pathlib.Path(__file__).resolve().parent.parent
FSD  = ROOT / 'docs/Functionality/gPlug-mini-FSD.md'
PLAN = ROOT / 'testing/test-plan.yaml'
MARK = {'successful': 'PASS', 'failed': 'FAIL', 'not done': '--'}


def load():
    plan = yaml.safe_load(PLAN.read_text())
    reqs = {}
    for line in FSD.read_text().splitlines():
        m = re.match(r'^\| \*\*(N?FR-[A-Z]+-\d+)\*\* \| (\w+) \| ([^|]+)\|', line)
        if m:
            reqs[m.group(1)] = (m.group(2), m.group(3).strip())
    cases = plan['test_cases']
    atomic = [t for t in cases if t.get('kind') == 'atomic']
    flows  = [t for t in cases if t.get('kind') == 'workflow']
    return reqs, plan['capabilities'], atomic, flows, plan.get('release_gates', {})


def blocked_by(test, caps):
    return [n for n in test.get('needs') or [] if n in caps and not caps[n]['available']]


def status_of(test, caps):
    """A status the reader can act on, which is not always the recorded one."""
    if test['status'] == 'successful':
        return 'PASS', ''
    if test['status'] == 'failed':
        return 'FAIL', test.get('reason') or ''
    miss = blocked_by(test, caps)
    if miss:
        return 'BLOCKED', 'needs ' + ', '.join(miss)
    return '--', test.get('reason') or 'not written'


def by_requirement(reqs, caps, tests):
    idx = collections.defaultdict(list)
    for t in tests:
        for r in t['verifies']:
            idx[r].append(t)
    print(f"{'Requirement':12} {'Test':8} {'Tier':7} {'Status':8} Note")
    print('-' * 96)
    for r in sorted(reqs, key=lambda x: (x.split('-')[1], x)):
        for i, t in enumerate(idx.get(r, [])):
            st, note = status_of(t, caps)
            print(f"{r if i == 0 else '':12} {t['id']:8} {t['tier']:7} {st:8} {note[:52]}")
        if r not in idx:
            print(f"{r:12} {'—':8} {'':7} {'NO TEST':8}")


def by_tier(reqs, caps, tests):
    for tier in ('host', 'target', 'bench', 'field', 'other'):
        group = [t for t in tests if t['tier'] == tier]
        if not group:
            continue
        done = sum(1 for t in group if t['status'] == 'successful')
        print(f"\n=== {tier}  ({done}/{len(group)} passing) " + '=' * (54 - len(tier)))
        for t in sorted(group, key=lambda x: x['id']):
            st, note = status_of(t, caps)
            print(f"  {t['id']:8} {st:8} {','.join(t['verifies']):22} {t['does'][:44]}")


def gaps(reqs, caps, tests):
    verified = {r for t in tests if t['status'] == 'successful' for r in t['verifies']}
    print(f"{len(verified)} of {len(reqs)} requirements have a passing test\n")
    why = collections.Counter()
    for t in tests:
        if t['status'] == 'successful':
            continue
        st, note = status_of(t, caps)
        why[note.split('—')[0].strip() or 'not written'] += 1
    print('why the rest are not verified:')
    for k, v in why.most_common():
        print(f'  {v:4}  {k[:70]}')
    print('\nrequirements with no passing test at all:')
    for r in sorted(set(reqs) - verified, key=lambda x: (x.split('-')[1], x)):
        print(f'  {r:12} {reqs[r][1][:70]}')


def balance(atomic, flows):
    """The four scenarios and their share, atomic and workflow kept apart.

    Printed on every report because the tilt is what goes wrong quietly:
    negatives accumulate one defect at a time, each of them justified, and
    nobody notices the standard case was never written. This plan reached 108
    tests with the standard meter journey still unwritten.
    """
    c = collections.Counter(t.get('scenario', '?') for t in atomic)
    total = sum(c.values())
    print('balance of atomic scenarios')
    for k in ('standard', 'deviation', 'negative', 'security'):
        print(f'  {k:10} {c[k]:4}  {100 * c[k] // total:>3}%')
    if c['standard'] + c['deviation'] < c['negative'] + c['security']:
        print('  ** more fault-finding than function — the standard case is under-tested')
    done = sum(1 for t in atomic if t['status'] == 'successful')
    wdone = sum(1 for w in flows if w['status'] == 'successful')
    print(f"\n  {done}/{len(atomic)} atomic cases passing"
          f"   ·   {wdone}/{len(flows)} workflows passing")
    if wdone == 0 and done:
        print('  ** every component result is isolated — no journey has been proven end to end')
    print()


def workflows(caps, flows):
    """Workflows on their own, because a green component says nothing about them."""
    print(f"{'Workflow':8} {'Role':16} {'Tier':6} {'Status':8} Proves")
    print('-' * 96)
    for w in sorted(flows, key=lambda x: x['id']):
        st, note = status_of(w, caps)
        print(f"  {w['id']:6} {w.get('role', '—'):16} {w['tier']:6} {st:8} {w['name'][:44]}")
        if st != 'PASS':
            print(f"  {'':6} {'':16} {'':6} {'':8} {note[:70]}")


def gates(caps, flows, gate_defs):
    """What a tag would be asserting, resolved against the current baseline."""
    by_id = {w['id']: w for w in flows}
    for name, gate in gate_defs.items():
        req = gate.get('requires', {}) or gate.get('additionally_requires', {}) or {}
        wanted = req.get('workflows', [])
        print(f"\n=== {name}  ({gate.get('status', '?')}) " + '=' * (58 - len(name)))
        if gate.get('inherits'):
            print(f"  inherits {gate['inherits']}")
        for wid in wanted:
            w = by_id.get(wid)
            if not w:
                print(f"  {wid:8} UNKNOWN — the gate references a workflow that does not exist")
                continue
            st, note = status_of(w, caps)
            print(f"  {wid:8} {st:8} {w['name'][:40]:42} {note[:28]}")
        blockers = [w for wid in wanted if (w := by_id.get(wid))
                    and status_of(w, caps)[0] != 'PASS']
        print(f"  → {len(wanted) - len(blockers)}/{len(wanted)} satisfied"
              + ('' if not blockers else f", {len(blockers)} blocking"))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--tier', action='store_true')
    ap.add_argument('--gaps', action='store_true')
    ap.add_argument('--gates', action='store_true')
    a = ap.parse_args()
    reqs, caps, atomic, flows, gate_defs = load()
    balance(atomic, flows)
    if a.gates:
        gates(caps, flows, gate_defs)
    else:
        (by_tier if a.tier else gaps if a.gaps else by_requirement)(reqs, caps, atomic)
        if not a.gaps:
            print()
            workflows(caps, flows)
