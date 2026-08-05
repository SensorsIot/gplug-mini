#!/usr/bin/env python3
"""Render testing/test-plan.yaml into the report.

Nothing here is authored. Requirements come from the FSD, tests from the plan,
results from whatever ran them. The report is a view, so it cannot disagree with
its sources - which is the whole reason coverage is not a hand-kept column.

    tools/report.py            group by requirement
    tools/report.py --tier     group by tier
    tools/report.py --gaps     only what is not verified
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
    return reqs, plan['capabilities'], plan['tests']


def blocked_by(test, caps):
    return [n for n in test['needs'] if n in caps and not caps[n]['available']]


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


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--tier', action='store_true')
    ap.add_argument('--gaps', action='store_true')
    a = ap.parse_args()
    reqs, caps, tests = load()
    (by_tier if a.tier else gaps if a.gaps else by_requirement)(reqs, caps, tests)
