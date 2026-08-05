# Test procedures

Setup, teardown and evidence rules shared by the tests in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml). A test names one of
these in its `procedure` field rather than repeating it, because a procedure
copied into sixty-one tests is a procedure maintained in none of them.

Bench addresses are not written down here — there are several benches and they
move. gPlug-mini uses **Workbench2**; discover it before a session and confirm
with `GET /api/info`.

---

## Rules that hold for every procedure

**Capture evidence before resetting anything.** A reset destroys the reason for
the reset. Read the serial log, the reset reason and the simulator's `status`
line first; recover second.

**Drain the serial buffer before you start.** The workbench's monitor returns
buffered output, and lines from different times arrive spliced — during this
project's first session a captured line turned out to be two messages joined,
with timestamps forty minutes apart, one of them from before a reboot. A test
that matches a phrase without draining first can match something a previous test
said.

**A timeout is a failure, not an absence of result.** Nothing on the device
returns an exit code; all you get is text. A board that crashes, hangs or resets
mid-test prints no failure message at all, so "no FAIL seen" must never be read
as a pass. Require a positive completion marker and treat its absence as failure.

**Leave the rig as you found it.** A fault directive left set silently corrupts
every later test. Each procedure below states its own teardown.

---

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
