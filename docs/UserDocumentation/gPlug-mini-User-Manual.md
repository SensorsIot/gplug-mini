# gPlug-mini — User Manual

How to install and run gPlug-mini. Human procedures, present-state. Step-by-step
operational detail lives here and nowhere else — the
[FSD](../Functionality/gPlug-mini-FSD.md) says what the device does, this says how
you make it do it.

**Status: nothing is deployable yet.** No firmware exists; every requirement is at
*Specified*. The chapters below are the shape this manual takes once there is
something to install, and each names the requirement it will draw from.

This file exists from the first commit deliberately. An OPERATE plane that was
never created is invisible — nobody misses a directory that does not exist, and
the first person to install the device would find there are no instructions at
exactly the moment they need them. A stub that says "not written yet" is a visible
gap with a due date.

Everything operational belongs in this one manual, as a new chapter. A second
operations document beside it splits the reader's starting point in two.

## Contents

1. [What you need](#1-what-you-need)
2. [Installing the firmware](#2-installing-the-firmware)
3. [Mounting at the meter](#3-mounting-at-the-meter)
4. [Provisioning](#4-provisioning)
5. [Configuration reference](#5-configuration-reference)
6. [Reading the indicators](#6-reading-the-indicators)
7. [Home Assistant](#7-home-assistant)
8. [Updating the firmware](#8-updating-the-firmware)
9. [Reconfiguring](#9-reconfiguring)
10. [Troubleshooting](#10-troubleshooting)
11. [Recovery](#11-recovery)

## 1. What you need

*Pending Phase 2.* The gPlugM adapter, an RJ45 cable to the meter's customer
interface, and a phone or laptop for provisioning. Nothing else — the meter
supplies power.

## 2. Installing the firmware

*Pending Phase 2.* Flashing over USB through the Embedded Workbench, before the
device goes into the cabinet.

The partition layout is fixed at this moment and cannot change afterwards without
USB access — see FSD Appendix C and `OD-4`. Confirm the flash size before the
first flash, not after.

## 3. Mounting at the meter

*Pending Phase 2.* The RJ45 pinout and which meter pins carry data and power are
in the [interface spec §2.1](../Functionality/MBUS-E450-Interface-Spec.md). The
device is unpowered whenever the meter is.

## 4. Provisioning

*Pending Phase 2.* The device raises a WPA2 access point on first boot. Its
passphrase is derived from the MAC address — `FR-PRV-02` defines the rule, and
this chapter states it in a form you can compute by hand.

## 5. Configuration reference

*Pending Phase 2.* The configurable set is FSD §17: WiFi credentials, broker host
and port, MQTT credentials, and an optional DLMS key. Everything else has a
correct default and is not exposed.

## 6. Reading the indicators

*Pending Phase 2.* The three LEDs are the only diagnostic channel available
without opening the cabinet — remote log access is deliberately out of scope
(FSD §4.3), so this chapter carries more weight here than it would on a device
with a console. State-to-pattern mapping is FSD §11.3.

## 7. Home Assistant

*Pending Phase 2.* Entities appear on their own through MQTT discovery. Covers
which entities to expect, how to add the energy ones to the Energy Dashboard, and
why the history follows the meter rather than the device.

## 8. Updating the firmware

*Pending Phase 2.* Publish a release URL to the OTA command topic. The device
downloads, reboots, and keeps the new image only once it reaches the broker — so
a build that cannot connect reverts by itself.

## 9. Reconfiguring

*Pending Phase 2.* Hold the button for five seconds to reopen the portal. It
times out back to normal operation, so a stray press cannot strand the device.

## 10. Troubleshooting

*Pending Phase 2.* Symptom → cause → fix. Every failure that costs someone an
hour earns a row.

## 11. Recovery

*Pending Phase 2.* What happens automatically — WiFi retry, OTA rollback,
watchdog reset — and what needs a person. The design intent is that nothing here
requires opening the meter cabinet except the first flash; FSD §21 states that as
a procedure so a deviation is visible.

---

**Contract:** [`../Functionality/gPlug-mini-FSD.md`](../Functionality/gPlug-mini-FSD.md)
· **Build rules:** [`../Harness/`](../Harness/)
