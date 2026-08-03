# gPlug-mini — Harness (HOW)

The Harness is the **build contract**: the rules for how this project is built and
changed. Entry point for any task: **[`AI-Workflow.md`](AI-Workflow.md)**.

## Layout

| Path | Contents |
|------|----------|
| [`AI-Workflow.md`](AI-Workflow.md) | The loop every change follows. Read it before touching code. |
| [`standards/`](standards/) | Portable rules, reusable on any project — engineering, testing, documentation governance. |
| [`project/`](project/) | gPlug-mini's own bindings: layers, source layout, dependency rules, prohibitions, tool pointers. |

## The three planes

| Plane | Question | Document |
|-------|----------|----------|
| **WHAT** | What must be true of the device? | [`../gPlug-mini-FSD.md`](../gPlug-mini-FSD.md) |
| **HOW** | How is it built and changed? | This directory |
| **OPERATE** | How do I install and run it? | *(no Handbook yet — nothing is deployable)* |

**Authority order: the FSD defines the target; the Harness defines the method.**
On conflict the FSD wins on *what must be true*, the Harness wins on *how to get
there*. A Handbook, when one exists, describes the system as built — if it
disagrees with either, it is stale.

Two further documents are inputs to the FSD rather than planes:

- [`../MBUS-E450-Interface-Spec.md`](../MBUS-E450-Interface-Spec.md) — what the
  meter and the board actually do. Facts about hardware we do not control.
- [`../decisions.md`](../decisions.md) — settled design decisions with provenance,
  and the alternatives rejected. Consult before proposing a change that reverses
  one; §4 exists to stop good ideas being re-litigated every few months.

## What belongs here

A rule belongs in the Harness when it constrains **how the code is written,
structured, or verified** without being observable from outside the running
system. If a black-box tester could verify it, it is a requirement and belongs in
the FSD instead.

- Belongs here: "one module per component", "lower layers never import higher
  ones", "extract pure cores for host testing".
- Belongs in the FSD: "publishes within 2 s of a cycle boundary", "never enters
  AP mode on a WiFi drop".
- Belongs in a Handbook: "hold the button for five seconds to reconfigure".

Two questions settle the hard cases. *Could a black-box tester verify it?* — yes
means WHAT. *Would it survive a rewrite in another language?* — no means HOW.

## Documentation rule

All planes are present-state and single-home: state a fact once, link to it from
anywhere else. See [`standards/documentation.md`](standards/documentation.md).
