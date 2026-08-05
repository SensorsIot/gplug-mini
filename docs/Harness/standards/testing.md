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
- **A bench pass is a few proper signals decoded, not every one.** The M-Bus
  simulator is not a meter and does not pretend to be: it drops bytes, and
  chasing that is not this project's work. Assert across several cycles and
  require one good one. What the rig loses is recorded against the *capability*,
  never against the firmware — a rig characteristic filed as a defect sends the
  next person hunting code that is doing nothing wrong.
- **Assert the prohibited outcome, not just the happy one.** The FSD's verification
  contracts carry a *Must NOT happen* column for a reason: a rollback test that
  only checks "did it recover" passes when the device recovers by rebooting, which
  is the failure being tested for.

## Test framework

**Host tier: CMake and CTest, C++20, under `testing/host/`.** The same language and
standard as the firmware, so a host test exercises the code that ships rather
than a reimplementation of it — a suite in another language can only test a
second version of the logic, which is the one kind of test that cannot fail
usefully. No test framework is pulled in: assertions and one CTest entry per case
are enough, and each case names the property that breaks rather than reporting
"the decode test failed".

`dlms_parser` is fetched by tag, matching the version the firmware pins, because
these tests exist to notice when its behaviour changes. A branch would let it
move underneath them.

```bash
cmake -S testing/host -B build/host && cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

**Target tier: Unity**, which ESP-IDF bundles.

## The release gate

Bench tests need the Embedded Workbench, which a GitHub-hosted runner cannot
reach. **Tagging is the human assertion that the bench suite passed** (FSD §20.2).

This is a deliberate position rather than a missing gate, and it carries an
obligation: run the bench suite before tagging. Automating it would require a
self-hosted runner on the bench network, which this project does not have.

After flashing the production build on the bench, confirm it boots, connects WiFi
and MQTT, and reports no meter data. That is the only check covering the seam the
simulated build cannot exercise.

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

### Driving a device

Flashing and bench testing go through the Embedded Workbench's HTTP API. **Do not
SSH into the workbench Pi to operate it** — every operation has an endpoint, and
reaching for SSH means the API is missing a capability that should be added there.

| Purpose | Endpoint |
|---|---|
| Flash over USB | `POST /api/flash` with `bin@<offset>` parts |
| Firmware update to a deployed board | `POST /api/ota` |
| MQTT broker for bench tests | `POST /api/mqtt/start` |
| WiFi AP with captive-portal provisioning | the workbench WiFi endpoints |
| Serial console | the RFC2217 proxy |

Bench tests are Python and drive these endpoints; host tests are C++ and touch
nothing outside the process.

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
- **Leave the rig as you found it, in every field — not only the one you
  changed.** A restore that resets the fault but not the serial length leaves the
  next test a longer telegram than it was written for, and that test fails for a
  reason that has nothing to do with the device. Assert the restored state rather
  than issuing the commands and assuming.

## Run order

Ordered by **what a test proves**, not by what it costs. A failure early makes
everything after it unreadable, and that matters more than minutes.

| Phase | Proves | Example |
|---|---|---|
| **1 host** | The logic, offline | Decode, OBIS mapping, cycle arithmetic |
| **2 bread-and-butter** | The device does its job | A measurement reaches the broker |
| **3 deviation** | Normal operation, not the simplest case | Either serial length; waking mid-transmission |
| **4 exception** | Faults and malfunctions | Corrupted frame, silent line, outage, rollback |

**Phase 2 is the gate, and its absence is a real risk.** A suite can accumulate
deviation and malfunction cases and never assert that the product works — this
one did, for a day, while a defect that stopped the device publishing entirely
was found sideways through an unrelated test that could not name the device's
topic. Write the bread-and-butter test first even though it feels too obvious to
write down.

**A deviation is not a malfunction.** Both meter serial lengths are normal
configurations; waking mid-transmission is what happens on every power-up, and
the interface spec calls resynchronisation an operating mode rather than an error
path (§4.1). Filing those under faults hides the fact that nothing tests the
ordinary case.

Cost is the tiebreaker *inside* a phase, never across them: cheap before
expensive, and anything that resets the board or injects a fault last within its
phase, so quieter tests do not pay for the recovery. Tests that prove a
negative — *no download happens* — cost their whole window every time and can
only confirm nothing, so they sort late.

Tests that prove a negative — *no download happens*, *nothing is published* —
are the worst of both: they cost their whole window every time and can only ever
confirm nothing. They go last.

Enforce this in the harness rather than by keeping the file in a tidy order. In
`testing/bench/` the markers are `gate`, `fast`, `slow` and `disruptive`, and
`conftest.py` sorts on them; an unmarked test runs with the fast ones, because
assuming a new test is a gate would give it authority to skip the run.

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
