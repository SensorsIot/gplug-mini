# Standard — Testing

Testing is part of the build contract, not an afterthought. Every behaviour rule
in the FSD is verifiable, and every change ships the test that proves it.

This document is the whole of testing in the Harness: the rules, and the
procedures for running a session. It is one file on purpose — the rules and the
procedures kept restating each other when they were two.

| | |
|---|---|
| What must be proved | the FSD, one contract per requirement |
| Which tests exist, what they need, what they produced | [`testing/test-plan.yaml`](../../../testing/test-plan.yaml) |
| How to write and run them | here |

## Tiers

Use the cheapest tier that can catch the failure. Reserve the expensive ones for
behaviour no cheaper tier can reach.

| Tier | Runs on | Speed | Catches |
|------|---------|-------|---------|
| **host** | Dev machine, plain compiler | ms, every push | Decode, OBIS mapping, scaling, cycle-boundary arithmetic, supervisor transitions |
| **target** | The ESP32-C3 alone | seconds | UART configuration, NVS, GPIO, boot behaviour |
| **bench** | Device on the Embedded Workbench with AP, broker and OTA relay | minutes | Provisioning, sessions, discovery, OTA, rollback, watchdog, resilience |
| **field** | Installed at the meter | once | Physical layer, real frames, meter power, WiFi coverage |
| **other** | CI or review | — | Artefact properties not observable at runtime |

**Bench and field differ by control, not by realism.** On the bench the peers are
ours and faults can be injected — stop the broker, drop the access point, serve an
untrusted certificate. In the field the peers are real and can only be observed.
So every fault-injection case belongs on the bench, and the field answers only
what nothing else can.

**The field tier is optional, and this project's is small because an M-Bus
simulator exists** (`decisions.md` T7). Decode and protocol cases sit at bench;
the field tier keeps only radio coverage and meter power, which nothing in the
lab reproduces. Field cases are the most expensive kind — one-shot, rarely
repeatable — so anything that can leave the tier should.

**Four tiers are execution environments: host, target, bench, field.** A test
spec's `tier` is one of those four. `other` is **not a tier** — it is the bucket
for properties no environment can observe, checked by CI or review, and a case
lands there only when no device runs at all.

Layer mapping: **L0** is exercised transitively and has no host tests of its own —
empty cells there are expected, not gaps. **L1** interfaces split, with pure cores
at host and wire behaviour at target or bench. **L2** is host throughout.

## Rules

- **Every Must/Should rule has a test case.** A rule with no case is an untested
  contract — surface it as a gap rather than leaving it silent.
- **Every change ships its test.** Bug fixes write the regression test **first**,
  and it must fail before the fix.
- **Test cases name the requirement IDs** they verify, so coverage is computed
  rather than asserted.
- **Coverage is generated, never hand-maintained.** A hand-filled "Covered / GAP"
  column is stale the moment a test changes.
- **The supervisor is covered per transition, not per state.** The state model has
  32 rows; reaching `OPERATIONAL` proves nothing about the four edges into it.
- **A test must be able to fail.** One that asserts a payload has the right keys
  passes with the meter disconnected. Prefer one test that proves the thing works
  over several that assert shape.
- **Never tune a threshold to make a test pass.** A failing test is evidence until
  proven otherwise; find the defect first.
- **Assert the prohibited outcome, not just the happy one.** The FSD's verification
  contracts carry a *Must NOT happen* column for a reason: a rollback test that
  only checks "did it recover" passes when the device recovers by rebooting, which
  is the failure being tested for.

## Test data

The host tier runs against published E450 captures, committed as fixtures.

A capture taken from the real meter carries the meter serial, which identifies an
address and an account, and a power trace, which shows occupancy. Substitute the
serial consistently before committing one.

## Security testing

The threat profile is in FSD §18.1 and is deliberately modest: a device in a
private basement, on a home network. Cases derive from that profile rather than
being imported wholesale.

- A security criterion must be able to **fail**. "Credentials are protected" cannot;
  "the literal passphrase does not appear in any portal response body" can.
- Where the profile does not justify a protection, it is recorded as an accepted
  risk in FSD §4 rather than written as a requirement nobody implements. Plaintext
  NVS is the worked example: `NFR-SEC-01` states the non-claim explicitly.

---

# Running a test

## Rules that hold for every run

- **Capture evidence before resetting anything.** A reset destroys the reason for
  the reset. Read the serial log, the reset reason and the rig's own status first;
  recover second.
- **Drain the serial buffer before starting.** The workbench monitor returns
  buffered output and lines from different times arrive spliced — one captured
  here turned out to be two messages joined, timestamps forty minutes apart, one
  from before a reboot. A test that matches a phrase without draining can match
  something a previous test said.
- **A timeout is a failure, not an absent result.** Nothing on the device returns
  an exit code; a board that crashes or hangs prints nothing at all, so "no FAIL
  seen" is not a pass. Require a positive completion marker.
- **Verify the precondition, and if it fails record `not done`, never `failed`.**
  A failure claims you learned something about the requirement; with a broken
  baseline you learned nothing, and the next person hunts the wrong defect.
- **Leave the rig as you found it.** A fault directive left set silently corrupts
  every later test.

Bench addresses are not written down. gPlug-mini uses **Workbench2**; discover it
and confirm with `GET /api/info` before a session.

## P-TARGET — the board alone

**Needs** `board`.

Nothing else may be attached or running: no simulator traffic, no broker. The
point of this tier is that the chip is answering about itself.

**Before** — flash the test firmware once. All target cases live in a single
image and are selected by name over serial, so the five-minute build-and-upload
cost is paid per change, not per test.

**Steps** — send the case name; read until the run announces it finished and how
many checks it made.

**After** — nothing to restore.

**On failure** — capture the full serial output and the reset reason before
reflashing. A crash loop that is reflashed away takes its cause with it.

---

## P-METER — the meter link

**Needs** `board`, `mbus-sim`, usually `mqtt-broker`.

**Before**
1. Confirm the simulator is in a known state: `mode 3`, `fault none`, and the
   identity it should be emitting. It has been found set to `identity none`,
   which makes every discovery test fail for a reason that has nothing to do
   with the device.
2. Start mosquitto on **both** benches. Both broadcast `gplug-bench` and both
   gateways are `10.42.0.1`; the board joins whichever radio is stronger and
   cannot tell you which. Start one and the board may land on the quiet one and
   report only a TCP error.
3. Confirm the board is publishing, or that its failure to publish is the thing
   under test.

**Steps** — issue the simulator directive, then watch the board. Directives
apply to one telegram and clear themselves.

**After** — `fault none`, `mode 3`, `gap 0`.

**The rig is not stable, and this is accepted.** Measured with the counted ramp:
two bursts in six arrive complete and byte-perfect, four arrive corrupted from
the first byte. So **assert across several cycles and require one good one.**
A single-cycle assertion is flaky by construction and will be blamed on the
firmware.

**When a result is confusing, use `mode 4`.** The counted ramp makes the board's
first received byte the number of bytes it lost, directly. It replaced an
afternoon of inference with one reading, and it did it by contradicting the
theory we had built.

---

## P-NETWORK — WiFi, portal and broker

**Needs** `board`, `wifi-ap`, `mqtt-broker`; fault injection also needs
`wifi-ap-outage`, which does not exist yet.

**Before** — confirm the board is associated and holding a broker session, so a
later loss is attributable to the stimulus rather than to the starting state.

**Steps** — cut what the test needs cut, then watch. To interrupt MQTT for one
device without disturbing others, prefer a source-scoped firewall rule on the
broker host over stopping the broker: `DROP` produces the silent black hole that
exercises the keepalive timeout, `REJECT` produces an immediate refusal. They are
different tests.

**After** — restore the access point and the broker; confirm the board recovers
before starting the next case.

**Judge recovery by live publishing, never by a retained value.** A retained
topic read during or just after an outage returns the copy from before the
device dropped, with its counters frozen — it looks like a failure to recover
when nothing has recovered *yet*. Subscribe for longer than the publish cadence
and require at least two arrivals: the retained copy, then a fresh one.

**A last-will topic is death-only.** Nothing ever writes `LWT = online`; the will
stays `offline` from the last drop until the next. Watch the birth or status
topic for recovery, not the will.

---

## P-OTA — firmware update

**Needs** `board`, `ota-relay` — **which does not exist**, so all eleven OTA
tests are blocked. This procedure is written so that building the relay is the
only thing standing between them and running.

**Before** — record the running version and the active slot. Confirm a broker
session, since validation depends on one.

**Steps** — publish the command carrying the image URL; watch the download, the
write to the inactive slot, the reboot, and whether the image is marked valid.

**After** — return the board to a known-good image and confirm it boots and
connects.

**Rollback tests must observe the prohibited outcome, not just recovery.** A
device that recovers *by rebooting into the same broken image* has satisfied
"did it come back" and failed the requirement. Read the reset reason and the
running slot, not just the fact that something is alive.

---

## P-FIELD — the installed meter

**Needs** `real-meter`. One test, run once, in the cabin.

**Before** — the device must already pass everything the bench can reach. A
field visit spent finding a bench-tier fault is a wasted visit.

**Steps** — compare what the device publishes against the meter's own display.

**After** — nothing to restore, and nothing to repeat: this tier is one-shot by
nature, which is why so little is allocated to it.

**This is the only tier that can invalidate the bench.** The simulator emits
bytes we authored, so it proves the device reads what it is given and can never
prove the meter gives what we assumed.
