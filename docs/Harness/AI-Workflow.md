# gPlug-mini — AI Workflow (the build contract)

Read this before any change. It is how new functionality is built and how the
documentation stays in sync with it.

## The loop

**1. Locate the contract.** Find the FSD rule the work serves in
[`../Functionality/gPlug-mini-FSD.md`](../Functionality/gPlug-mini-FSD.md). Requirements carry stable IDs
(`FR-MTR-07`, `NFR-HA-01`) — cite the ID in the commit. If no rule covers the
work, it starts by defining the **WHAT**: a new atomic, falsifiable requirement,
not code. A change with no rule behind it is either scope creep or an
undocumented requirement, and both need resolving before implementation.

Before proposing something that reverses a settled decision, read
[`../decisions.md`](../decisions.md) §5 — ten alternatives are recorded there with
the reason each was dropped. Several look like obvious improvements in isolation.

**2. Build per the Harness.** Follow [`standards/`](standards/) and
[`project/`](project/). Reuse before adding. Make the smallest change that
satisfies the rule — no speculative scope, no drive-by refactors.

**3. Test — the gate, not an afterthought.** A change is **not done** until its
test exists and passes. Per [`standards/testing.md`](standards/testing.md): put
the case at the cheapest tier that can catch the failure, name the requirement ID
it verifies, and run the suite green. A bug fix writes its **regression test
first**, and that test must fail before the fix and pass after — a regression test
that never failed proves nothing.

**4. Reconcile the documentation.**
- The **FSD** absorbs new or changed behaviour — *verify, don't transcribe*. If
  the code deviates from the spec, fix the code; do not enshrine the defect as a
  requirement.
- The **Harness stays put** unless the change taught a rule that is *universally*
  true for this project, not just for this change.
- All present-state. No "now uses", no "previously".

**5. Verify both directions.** Confirm the implementation matches the FSD, and
that no FSD rule is silently unimplemented. Deviations fix the code; genuine gaps
are documented as gaps; contradictions are escalated rather than guessed at.

## Requirement quality gate

Before a new requirement enters the FSD it must be:

- **Atomic** — one obligation. Split anything joining two verbs, a behaviour and
  a deadline, or a success and a failure path.
- **Falsifiable** — precondition, stimulus, observable response, deadline,
  tolerance, failure behaviour, verification tier.
- **Free of weasel words** — *appropriate, graceful, user-friendly, reasonable,
  sufficient, robust, seamless, acceptable, normal operation, best effort*.
- **Provenance-tagged** — `[user]`, `[derived]`, `[code]`, `[pack:esp32]`.

## Closing an open decision

The FSD metadata block carries six open decisions (`OD-1`..`OD-6`), each naming
the requirement it gates. Closing one is a documented act, not a side effect:

1. Record the answer and how it was established.
2. Update every requirement the decision gated — the FSD names them.
3. Remove it from `open_decisions` and bump `fsd_version`.
4. Commit the FSD change with the code that depends on it, so the two never
   disagree in the history.

Four of the six close on a laptop, before hardware exists. `OD-4` (flash size)
must close before the first flash — after that the partition table cannot change
without USB access to a device in a meter cabinet.

## Deploying to a device

The device is flashed and driven through the Embedded Workbench, over its HTTP
API. See [`project/conventions.md`](project/conventions.md) for the endpoints and
the constraint that goes with them.

## Roles

The FSD is the contract; changing it changes what the product owes. Changing the
**Harness** is a deliberate act, because it re-scopes every future change — state
what rule is being added and why it is universal to this project rather than local
to the task in hand.
