# Standard — Documentation governance

All project documentation is **present-state**, lives in **one canonical home**,
and is routed by the **WHAT / HOW / OPERATE** triage.

## The three rules

**Present-state.** Write what is true now, in the present tense. No history, no
rationale narrative, no temporal comparison. Delete on sight: "now uses",
"previously", "as of v2", "we decided to", "legacy", "this was changed because".
Version history belongs in `git log`. The FSD's `change_history` metadata block is
the one sanctioned exception, because a historical test result has to be tied to
the exact spec revision that produced it.

**One canonical home.** Every fact is stated once; anywhere else that needs it,
link. Two copies of a fact are two facts the moment one is edited, and the reader
has no way to tell which is current. The FSD does not restate the interface spec;
the Harness does not restate the FSD.

**Routed by plane.** Before writing a sentence, decide which plane owns it:

1. Externally observable and must be true → **FSD**
2. Constrains how code is written or verified → **Harness**
3. Tells a human how to run or recover the device → **Handbook**
4. About collaborating with the AI assistant → `CLAUDE.md`, which is not a plane
5. Why a past decision was made → the commit message, or
   [`../../decisions.md`](../../decisions.md)

## Where the meter documentation sits

[`../../Functionality/MBUS-E450-Interface-Spec.md`](../../Functionality/MBUS-E450-Interface-Spec.md) is not a
plane. It records what the meter and the board do — facts about hardware nobody
here controls — and the FSD cites it rather than copying it. When the two
disagree, the interface spec wins on hardware behaviour and the FSD wins on what
the firmware owes.

It carries its own honesty rule, worth preserving in edits: each number is marked
as either a property of the interface or an implementation choice. Blurring the
two is how an arbitrary buffer size acquires the authority of a protocol constant.

## Review checklist

- [ ] No history, rationale narrative, or temporal words
- [ ] No fact stated in two places
- [ ] Every section is in the right plane
- [ ] All cross-references resolve
- [ ] No placeholder or `TODO` text left behind
