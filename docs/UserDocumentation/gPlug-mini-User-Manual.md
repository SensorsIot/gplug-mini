# gPlug-mini — User Manual

How to install and run gPlug-mini. Human procedures, present-state. Step-by-step
operational detail lives here and nowhere else — the
[FSD](../Functionality/gPlug-mini-FSD.md) says what the device does, this says how
you make it do it.

**Status: provisioning works; the rest is still being written.** Chapter 4 below
describes behaviour demonstrated on hardware on 2026-08-05 — the portal, the
credentials, and what the device does with them. The chapters still marked
*Pending* are the shape this manual takes as each part is proved, and each names
the requirement it will draw from.

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

On first boot — and after any reset that leaves it without a stored
configuration — the device raises its own WiFi network and waits **five minutes**
for you to configure it. If nobody connects in that time it gives up and
restarts; power-cycle it to get another five minutes.

### 4.1 Join the device's network

Look for a network named **`gplug-` followed by six hex characters**, for example
`gplug-254a75`. Those six characters are the last three bytes of the device's MAC
address, so every unit has a different name and two units side by side cannot be
confused.

The passphrase is **`gplug` followed by those same six characters** — for
`gplug-254a75` the passphrase is `gplug254a75`.

That rule exists so the device stays recoverable with no label and no paperwork:
the network name is broadcast, and the name tells you the passphrase. It is a
setup network that exists for five minutes at a time, not a secret. The network
is WPA2 rather than open so that what you type into the form — your home WiFi
password among it — is not readable by anyone in range.

### 4.2 Fill in the form

Once joined, open any web address at all. `http://192.168.4.1/` works, but so
does anything else: the device answers every hostname while provisioning, so
most phones offer the page by themselves as a "sign in to network" notification.

The form asks for four things:

| Field | What to enter | Required |
|---|---|---|
| **WiFi network** | the name of your home network | yes |
| **WiFi password** | its password; leave empty for an open network | no |
| **Broker** | your MQTT broker as a full URI, e.g. `mqtt://192.168.0.10:1883` | yes |
| **Hostname** | a name for the device on your network; leave empty for the default | no |

The page lists the networks the device can see, so you can check the spelling of
yours against what it actually hears. A network name typed with one character
wrong fails later as a *wrong password*, which is the hardest possible way to
find a typo.

**The broker must include the `mqtt://` scheme.** An address on its own is
refused, and the page tells you so rather than accepting it and failing later —
by which point nothing would point back at this form.

### 4.3 What happens next

The device replies **Saved**, stores the configuration, and restarts. On the next
boot it joins the network you gave it and connects to your broker; the setup
network disappears. Nothing is written until the whole form is accepted, so a
rejected submission leaves the device exactly as it was, still waiting.

If it cannot join — wrong password, network out of range — it does **not** put
the setup network back up. That is deliberate: a device in a meter cupboard
raising an access point nobody can see is unreachable rather than recoverable.
Use chapter 9 to reconfigure it deliberately.

## 5. Configuration reference

*Pending Phase 2.* The configurable set is FSD §17: WiFi credentials, broker host
and port, and MQTT credentials. Everything else has a
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
