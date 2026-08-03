# gPlug-mini

![Platform](https://img.shields.io/badge/platform-ESP32--C3-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-6.0.2-red)
![Status](https://img.shields.io/badge/status-specification-lightgrey)

## The Problem

Your electricity meter already knows everything — how much you use, how much your
solar panels export, what each phase is drawing. A Landis+Gyr E450 pushes all of
it out of a socket on its front panel, several times a minute, for free.

Nothing you own can read it. The socket speaks a protocol built for utilities,
over wiring built for utilities, and the meter sits in a basement cabinet where
there is no screen, no keyboard, and no convenient way to get at anything.

## The Solution

gPlug-mini plugs into that socket and puts your meter into Home Assistant. It
decodes what the meter is saying and publishes it as ordinary sensors — so
consumption, export and per-phase readings show up in the Energy Dashboard with
nothing to configure on the Home Assistant side.

It is built to be left alone. Once installed it never needs the cabinet opened
again: it configures itself from a phone, survives router and broker outages
without help, and takes firmware updates over the network.

## What It Does

- Reads a Landis+Gyr E450 through its customer information interface — passively,
  never writing to the meter
- Publishes to Home Assistant via MQTT discovery: energy, power, voltage and
  current entities appear on their own
- Energy entities work in the Energy Dashboard, and their history follows the
  **meter** rather than the box — replace the hardware and your statistics
  continue
- Configured once from a phone browser through a captive portal
- Updates over the air, and reverts by itself if a new build cannot reach the
  broker
- Never falls back to access-point mode because WiFi dropped — a router reboot
  must not strand it in a portal nobody can see
- Runs on power supplied by the meter itself

## Status

**Specification complete; no firmware yet.** Every requirement is at *Specified*
— none is implemented or verified. See
[the FSD](docs/gPlug-mini-FSD.md) §22.2 for the verification lifecycle, and
§4 for open decisions.

## Documentation

| Question | Document |
|---|---|
| **What must be true of it?** | [Functional Specification](docs/gPlug-mini-FSD.md) — requirements, state model, verification contracts |
| **What does the meter actually do?** | [M-Bus / DLMS Interface Spec](docs/MBUS-E450-Interface-Spec.md) — physical, link and application layers of the E450 interface |
| **Why is it built this way?** | [Design Decisions](docs/decisions.md) — 37 decisions with provenance, and the alternatives that were rejected |

## Hardware

A gPlugM adapter (Gantrisch Energie AG) containing an ESP32-C3, connected to the
E450 customer interface by RJ45. The meter supplies 5 V; the M-Bus level shifter
is external to the MCU.

| Signal | GPIO |
|---|---|
| Meter data (UART RX, inverted, 2400 8E1) | 7 |
| LED red / blue / green | 1 / 3 / 4 |
| Button | 9 |

Full pinout and the meter-side connector mapping:
[interface spec §2–3](docs/MBUS-E450-Interface-Spec.md).

## Building

Firmware is compiled by GitHub Actions — a local ESP-IDF installation is not
required. Builds run on every push; releases are published only from a tag.

## License

Not yet chosen. The DLMS decoding library it depends on
([`esphome/dlms_parser`](https://github.com/esphome-libs/dlms_parser)) is
Apache-2.0.
