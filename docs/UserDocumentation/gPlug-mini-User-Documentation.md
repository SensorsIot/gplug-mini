# gPlug-mini — User Documentation (OPERATE)

How to install and run gPlug-mini. Human procedures, present-state. Step-by-step
operational detail lives here and nowhere else — the
[FSD](../Functionality/gPlug-mini-FSD.md) says what the device does, this says how
you make it do it.

**Status: nothing is deployable yet.** No firmware exists; every requirement is at
*Specified*. The sections below are the shape this document takes once there is
something to install.

This file exists from the first commit deliberately. An OPERATE plane that was
never created is invisible — nobody misses a directory that does not exist, and
the first person to install the device would discover there are no instructions at
exactly the moment they need them. A stub that says "not written yet" is a visible
gap with a due date.

## Access and prerequisites

*Pending Phase 2.* Will cover: the gPlugM adapter, the RJ45 cable to the meter's
customer interface, a phone or laptop for provisioning, and where the provisioning
passphrase comes from (derived from the MAC — `FR-PRV-02`).

## Installation and first run

*Pending Phase 2.* Will cover: flashing over USB via the Embedded Workbench,
mounting in the meter cabinet, and the first-boot provisioning flow. The partition
layout is fixed before the first flash and cannot change afterwards without USB
access — see FSD Appendix C and `OD-4`.

## Configuration

*Pending Phase 2.* The configurable set is FSD §17: WiFi credentials, broker host
and port, MQTT credentials, and an optional DLMS key. Everything else has a
correct default.

## Routine operations

*Pending Phase 2.* Will cover: reading the indicator states (FSD §11), triggering
a firmware update by publishing to the OTA command topic, and reconfiguring by
holding the button for five seconds.

## Diagnostics and troubleshooting

*Pending Phase 2.* Symptom → cause → fix. The indicators are the only diagnostic
channel available without opening the cabinet — remote log access is deliberately
out of scope (FSD §4.3), so this section carries more weight here than it would on
a device with a console.

## Recovery

*Pending Phase 2.* Will cover: what happens automatically (WiFi retry, OTA
rollback, watchdog reset) and what needs a person. The design intent is that
nothing in this section requires opening the meter cabinet except the first flash —
FSD §21 states that as a procedure so a deviation is visible.

---

**Contract:** [`../Functionality/gPlug-mini-FSD.md`](../Functionality/gPlug-mini-FSD.md)
· **Build rules:** [`../Harness/`](../Harness/)
