# Standard — Testing

Testing is part of the build contract, not an afterthought. Every behaviour rule
in the FSD is verifiable, and every change ships the test that proves it.

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
