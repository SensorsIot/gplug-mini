# gPlug-mini — Functional Specification Document (FSD)

```yaml
document_status:            draft
fsd_version:                0.3.0
repository:                 gplug-mini
baseline_commit:            1b4db45
applicable_firmware_version: 0.0.0-4f44d6b (bench)
author:                     SensorsIot
reviewers:                  —
approval_status:            pending review
created:                    2026-08-03
last_updated:               2026-08-05
change_history:
  - 0.1.0 · 2026-08-03 · initial specification from decisions.md and
    MBUS-E450-Interface-Spec.md
  - 0.2.0 · 2026-08-05 · verification contracts merged into the requirement
    tables; tier moved out of the FSD onto tests; requirements redistributed to
    the components that own them; two build-restating requirements removed
  - 0.3.0 · 2026-08-05 · every requirement carries a contract. FR-WDT-01
    weakened: it claimed every task is watchdog-subscribed, which no stimulus can
    observe, and now claims a subscribed task that stalls causes a reset within
    the configured period. Narrower on purpose - the wider claim needs a firmware
    seam listing subscribers, and an unverifiable requirement is worse than a
    smaller verifiable one
superseded_requirements:
  - FR-MTR-09 · split; the identity half became FR-MTR-10
  - FR-CFG-01..04, FR-ERR-01..04, NFR-NET-03 · moved to their owning components
  - NFR-NET-01, NFR-NET-02, FR-DEC-01, FR-DEC-02 · removed, restated elsewhere
open_decisions:
  - OD-6  WiFi coverage inside the meter cabinet (gates field acceptance)
related_test_baseline:      testing/test-plan.yaml
```

Requirement provenance is tagged `[user]`, `[derived]`, `[pack:esp32]`.
Hardware and protocol facts are **not restated here** — they live in
[`MBUS-E450-Interface-Spec.md`](MBUS-E450-Interface-Spec.md) and are cited by
section. Settled design decisions live in
[`decisions.md`](../decisions.md) and are cited by decision ID.

---

## 1. System Overview

### 1.1 Purpose

gPlug-mini reads a Landis+Gyr E450 electricity meter through its customer
information interface and presents the readings to Home Assistant as native
sensor entities, so household electricity consumption and production appear in
the Home Assistant Energy Dashboard without a cloud service or a utility portal.

### 1.2 Problem statement

The E450 pushes its measurements continuously on a wired interface that no
consumer device understands. The data is available, free, and unread. Reaching it
requires a device that sits permanently in the meter cabinet — a location with no
keyboard, no screen, and no convenient physical access.

### 1.3 Users

| User | Interaction |
|---|---|
| Householder | Sees electricity data in Home Assistant. Never interacts with the device after installation |
| Installer | Mounts the device, provisions it once through a phone browser |
| Maintainer | Issues firmware updates remotely; diagnoses faults without a site visit |

### 1.4 Goals

- Publish E450 measurements to Home Assistant with no manual configuration in
  Home Assistant.
- Survive network, broker and meter outages without intervention.
- Be updatable and diagnosable without opening the meter cabinet.

### 1.5 Non-goals

- Reading the sub-meter channels (gas, water, heat) the E450 relays. They are
  pushed but not decoded — interface spec §6.1.
- Controlling, configuring or writing to the meter. The device is a passive
  listener and never transmits on the meter link — interface spec §1.
- Operating on an encrypted customer interface. A key slot exists; decryption is
  not in this specification — §4.3 below.

### 1.6 System flow

```
E450 ──push──► gPlug-mini ──decode──► measurement set ──MQTT──► Home Assistant
                    │                                              │
                    └── captive portal (provisioning)              └── Energy
                    └── HTTPS pull (firmware update)                   Dashboard
```

---

## 2. System Architecture

### 2.1 Logical architecture

Four concurrent concerns share one MCU:

1. **Meter ingestion** — a receive-only UART accumulating bursts, decoded into a
   measurement set once per meter cycle.
2. **Connection supervision** — a state machine owning WiFi, the broker session,
   the provisioning portal, and the update window.
3. **Publication** — Home Assistant discovery once per session, measurement state
   on every completed cycle.
4. **Supervision** — a watchdog that recovers the device from a hang without
   human presence.

Meter ingestion is independent of network state: the meter is decoded whether or
not anything is listening, and a decoded set with nowhere to go is discarded
(FR-AGG-05).

### 2.2 Hardware / platform architecture

| Element | Value | Source |
|---|---|---|
| MCU | ESP32-C3 on a gPlugM adapter | Interface spec §3 |
| Meter link | UART RX only, GPIO7, 2400 8E1, inverted, no internal pulls | Interface spec §2.2 |
| Indicators | 3 LEDs, active high, GPIO 1 / 3 / 4 | Interface spec §3 |
| Input | 1 button, GPIO9, active low, internal pull-up | Interface spec §3 |
| Power | 5 V from the meter's customer interface | Interface spec §2.1 |
| Network | 2.4 GHz WiFi, station mode; SoftAP during provisioning | D-C3, D-C6 |
| Flash | 4 MB embedded (XMC, JEDEC 20/4016) | Read from the device |

**Power has an architectural consequence.** The device is energised only while
the meter is. It cannot outlive its data source, cannot report the meter being
unpowered, and starts listening mid-burst on every power cycle — which is why
mid-stream resynchronisation is a normal operating mode rather than an error path
(interface spec §4.1).

**GPIO9 is the ESP32-C3 boot strapping pin.** Held low through reset it forces
serial-download mode, so it is sampled only after boot (FR-PRV-04).

### 2.3 Software architecture

ESP-IDF 6.0.2. The application is C++ — the DLMS library is C++20 (D-D2).

| Concern | Realisation |
|---|---|
| Boot | `app_main` initialises NVS, then the supervisor state machine |
| Concurrency | FreeRTOS tasks: meter ingestion, connection supervision, publication |
| Persistence | NVS, plaintext (D-C2) |
| Update | Dual OTA slots with bootloader rollback (D-U3) |
| Build variants | Production and `-sim`, selected at compile time (D-T2) |

Four ESP-IDF 6 consequences apply and are silent until a build fails:

- `cmake_minimum_required` must be **3.22** or later.
- **cJSON is not bundled.** Home Assistant discovery payloads are JSON, so
  `espressif/cjson` is a required managed component.
- **mDNS is not bundled.** A broker address entered as `homeassistant.local`
  cannot resolve without `espressif/mdns`. See FR-PRV-08.
- Adding `idf_component.yml` to a configured build tree does nothing until the
  build directory is removed or `reconfigure` is run.

`dependencies.lock` is committed; `managed_components/` is not.

### 2.4 Component layering

Strict one-way dependency: each layer depends only on layers below it. The
L0/L1 line is **ownership** — code whose protocol logic this project wrote is an
interface; a library or platform service it merely configures is foundation.

```mermaid
flowchart TB
  subgraph L2["L2 — Application logic"]
    AGG["Reading Aggregator"]
    SUP["Connection Supervisor"]
  end
  subgraph L1["L1 — Interfaces"]
    MTR["Meter Interface"]
    HA["Home Assistant Publisher"]
    PRV["Provisioning Portal"]
    OTA["OTA Updater"]
    LED["Status Indicator"]
  end
  subgraph L0["L0 — Foundation / platform"]
    NET["WiFi STA"]
    MQ["MQTT client"]
    TLS["HTTPS / TLS"]
    NVS["NVS"]
    DEC["dlms_parser"]
    WDT["Task watchdog"]
    HTTPD["HTTP server"]
  end

  AGG --> MTR & HA
  SUP --> PRV & OTA & LED & HA
  L1 --> NET & MQ & TLS & NVS & DEC & WDT & HTTPD
```

```
┌─ L2 · Application logic ───────────────────────────────────────────
│   Reading Aggregator   ·   Connection Supervisor
│                            ▼ depends on
├─ L1 · Interfaces ─────────────────────────────────────────────────
│   Meter Interface · HA Publisher · Provisioning Portal ·
│   OTA Updater · Status Indicator
│                            ▼ depends on
├─ L0 · Foundation / platform ──────────────────────────────────────
│   WiFi STA · MQTT client · HTTPS/TLS · NVS · dlms_parser ·
│   Task watchdog · HTTP server
└────────────────────────────────────────────────────────────────────
```

**`dlms_parser` is L0, not L1.** It is a third-party library that owns the HDLC
and DLMS protocol logic; this project configures and calls it but neither
implemented nor tests it (D-D1). What *is* owned — UART configuration, feeding
bytes, cycle-boundary detection, and mapping OBIS codes to the measurement model
— is the Meter Interface at L1.

Source-layout rules that make an implementation mirror this layering are HOW, and
belong in the project's Harness, not here.

---

## 3. Implementation Phases

### 3.1 Phase 1 — Decode proven off-hardware

**Scope.** Build `dlms_parser` for the host and run it against published E450
captures.

**Deliverables.** Host test suite; the measurement mapping; a CI workflow running
host tests on every push.

**Exit criteria.** Every register in Appendix B decodes from a published capture
with the correct value, unit and scale.

**Dependencies.** None. No hardware.

### 3.2 Phase 2 — Device on the bench

**Scope.** Provisioning, WiFi, broker session, HA discovery and state, LED
behaviour, watchdog, the `-sim` build, and OTA including a rollback.

**Deliverables.** Both build variants; the bench test suite; a tagged release.

**Exit criteria.** Every bench-tier verification contract in this document passes
on the Embedded Workbench.

**Dependencies.** Phase 1. A gPlugM adapter on the bench.

### 3.3 Phase 3 — Cabin

**Scope.** Install at the meter. Confirm the physical layer, real frame decode,
meter-supplied power, and WiFi coverage.

**Deliverables.** A recorded capture from the real meter, kept as a regression
fixture.

**Exit criteria.** The acceptance scenarios in §22.1 pass in situ, and OD-6 is
closed.

**Dependencies.** Phase 2. Physical access, once.

---

## 4. Risks, Assumptions & Dependencies

### 4.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| WiFi does not reach the meter cabinet | Medium | Blocks deployment entirely | Explicit field test (§22.1). No firmware mitigation exists; the answer is an access point or an external antenna |
| Real frames differ from published captures | Medium | Decoder rework late | Decoding is proven against published captures before hardware; a real capture becomes a fixture in Phase 3 |
| Meter serial length differs from the assumption | Medium | Decoder finds no blocks and **fails silently** | Treated as a parameter, not a constant (FR-MTR-05); logged as a distinct condition (FR-MTR-14) |
| DSO changes the pushed register set | Low | Entities vanish from Home Assistant | Publish only what is present (FR-HA-05); absence is not an error |
| DSO enables encryption on the interface | Low | Total loss of readings | Key slot provisioned (FR-MTR-11); decryption itself is out of scope |
| Bad firmware reaches the device | Low | Physical visit required | Rollback on failure to reach the broker (FR-OTA-05); manual trigger only (FR-OTA-02) |

### 4.2 Assumptions

- The customer interface is unencrypted, as it is for Swiss deployments
  *(assumed from interface spec §4.3)*.
- The Home Assistant MQTT integration is installed and its discovery prefix is
  the default `homeassistant` *(assumed)*.
- Exactly one E450 is read per device *(assumed)*.

### 4.3 Explicitly out of scope

Recorded so a later reader sees a decision rather than an omission.

| Excluded | Reason |
|---|---|
| **Remote log access over MQTT** | Considered and declined. Diagnostics are the LED states of §11 plus the local serial console. Accepted consequence: a fault the LEDs do not describe requires physical access |
| DLMS decryption | No key, no encrypted meter to test against. Implementing a cipher before there is a key to test it with produces untestable code — interface spec §4.3 |
| Sub-meter channels (gas, water, heat) | Pushed by the meter, decoded by nobody here — interface spec §6.1 |
| Offline buffering across broker outages | `total_increasing` lets Home Assistant reconstruct consumption from the next counter value; a gap costs resolution, not correctness (D-H4) |
| Automatic update polling | Delivers a bad build everywhere before anyone notices (D-U2) |
| Flash encryption / encrypted NVS | Irreversible eFuse burning to protect a WiFi password from an attacker already inside the meter cabinet (D-C2) |
| Hardware meter simulator | The decoder is identical whether bytes arrive from a file or a wire; the wire adds only the physical layer, which the field tier confirms (D-T5) |
| Self-hosted CI runner gating on bench tests | The tag carries that meaning instead (D-B3) |
| ESP32-pack test families beyond the curated subset | ~150 cases, most asserting response shape rather than behaviour |

### 4.4 Dependencies

| Dependency | Version | Note |
|---|---|---|
| ESP-IDF | 6.0.2 | D-P4 |
| `esphome/dlms_parser` | ^2.1.0, Apache-2.0 | D-D1. Third-party, not vendor-supported |
| `espressif/cjson` | latest | Not bundled in IDF 6 |
| `espressif/mdns` | latest | Only if `.local` broker addresses are supported — FR-PRV-08 |
| Home Assistant | any with MQTT discovery | External |

---

# Part A — Application Logic (L2)

## 5. Reading Aggregation

### 5.1 Purpose & scope

Turns a stream of decoded DLMS values into one measurement set per meter cycle,
and decides when that set is complete enough to publish.

### 5.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-AGG-01** | Must | The device shall treat a gap of more than 2000 ms between received meter frames as the end of a transmission cycle. | Feed two frames separated by 1999 ms, then 2000 ms, then 2001 ms | No boundary at 1999 ms; boundary at 2000 ms and 2001 ms | A boundary fires while frames are still arriving | `[user]` D-T2 |
| **FR-AGG-02** | Must | The device shall assemble a single measurement set from all frames received within one cycle. | Feed a capture whose OBIS definitions and values fall in different frames | One set containing values from both frames | Values from only the first frame | `[derived]` |
| **FR-AGG-03** | Must | The device shall publish a measurement set exactly once per cycle boundary. | Feed one complete cycle | Exactly one publish call | Two publishes for one cycle | `[derived]` |
| **FR-AGG-04** | Must | When a register appears more than once within one cycle, the device shall retain the first occurrence and discard later ones. | Feed a capture with a register in two blocks and differing values | The first value is retained | The later value overwrites the earlier | `[user]` interface spec §5.3 |
| **FR-AGG-05** | Must | When no MQTT session is established at a cycle boundary, the device shall discard the measurement set without retaining it. | Broker stopped; feed a complete cycle | Set discarded, no queue growth | Memory growth across cycles; replay on reconnect | `[user]` D-H4 |
| **FR-AGG-06** | Must | The device shall discard a measurement set containing no decoded values. | Feed 20 bytes of noise, then silence | No publish | An empty or all-zero payload published | `[derived]` |

### 5.3 Decision logic

```
frame received ──► append payload to cycle buffer, reset gap timer
gap timer > 2000 ms ──► decode buffer
                        ├─ zero values  ──► discard            (FR-AGG-06)
                        ├─ no session   ──► discard            (FR-AGG-05)
                        └─ otherwise    ──► publish, reset     (FR-AGG-03)
```

Publication is not retried and no set is queued. A set that cannot be published
is lost by design (D-H4) — the next cycle supersedes it within seconds, and
energy counters are cumulative, so Home Assistant reconstructs consumption from
whichever counter value arrives next.

### 5.4 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

### 5.5 Constants

| Constant | Value | Rationale |
|---|---|---|
| Cycle gap threshold | 2000 ms | Clear of intra-burst spacing, far below the 5 s shortest push interval — interface spec §4.2 |
| Cycle buffer | 2048 bytes | Sufficient for observed bursts — interface spec §4.2 |

---

## 6. Connection Supervisor

### 6.1 Purpose & scope

Owns the device's operating mode. Every externally visible behaviour that depends
on "what is the device doing right now" derives from this state machine.

### 6.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-SUP-01** | Must | On boot with no stored WiFi credentials, the device shall enter Provisioning. | Erase NVS, boot | SoftAP appears within 10 s | Device connects to any prior network | `[user]` D-C3 |
| **FR-SUP-02** | Must | On boot with stored WiFi credentials, the device shall attempt to connect to the stored network. | Credentials stored, boot | Association attempt to the stored SSID | SoftAP appears | `[user]` D-C3 |
| **FR-SUP-03** | Must | On loss of the WiFi association while credentials are stored, the device shall retry connection indefinitely. | Remove the access point for 10 min with credentials stored | Connection attempts continue throughout, at the capped backoff | Attempts stop, or Provisioning is entered | `[user]` D-C4 |
| **FR-SUP-04** | Must | The device shall not enter Provisioning as a consequence of failing to connect to a stored network. | `OPERATIONAL`; power off the access point for 10 min | Device retries throughout | **SoftAP appears at any point** | `[user]` D-C4 |
| **FR-SUP-05** | Must | Successive reconnection attempts shall use an increasing interval capped at 30 s. | Deny association repeatedly; log attempt times | Intervals increase, then hold at 30 s ±2 s | Interval exceeds 30 s, or attempts stop | `[derived]` D-C4 |
| **FR-SUP-06** | Must | The device shall enter Provisioning when the button is held for 5 s while running. | `OPERATIONAL`; hold the button 5 s | SoftAP appears within 5 s | Device reboots; download mode entered | `[user]` D-C3 |
| **FR-SUP-07** | Must | The device shall leave Provisioning and resume connecting 300 s after entering it, when no credentials have been submitted. | Enter Provisioning with credentials stored; wait 300 s | Returns to `CONNECTING` | Remains in AP mode indefinitely | `[derived]` D-C3 |
| **FR-SUP-08** | Must | The device shall continue decoding meter frames in every state. | Broker stopped; feed a cycle | Decode occurs and is logged | Ingestion stops while offline | `[derived]` |
| **FR-SUP-09** | Must | The device shall treat WiFi SSID, WiFi passphrase and broker host as required, and enter Provisioning when any is absent. | Boot with each of SSID, passphrase and broker host absent in turn | Provisioning entered in each of the three cases | Operation attempted with an incomplete configuration | `[derived]` |
| **FR-SUP-10** | Must | The device shall accept an empty MQTT username and password. | Provision with MQTT username and password both left blank | A broker session is established | The portal rejects the form, or no session is attempted | `[derived]` |
| **NFR-SUP-01** | Must | The device shall re-establish an MQTT session within 60 s of the broker becoming reachable. | Stop the broker for 2 min, then restart it | A session is re-established within 60 s of the broker accepting connections | Recovery requires a device reset | `[pack:esp32]` |

### 6.3 State model (normative)

States:

| State | Meaning |
|---|---|
| `BOOT` | Initialising; NVS opened, configuration read |
| `PROVISIONING` | SoftAP up, captive portal serving |
| `CONNECTING` | WiFi association in progress or retrying |
| `LINKED` | WiFi associated; no MQTT session |
| `OPERATIONAL` | MQTT session established; publishing |
| `UPDATING` | OTA download in progress |

Transition table — every (state × event) pair is handled, ignored, or excluded:

| From | Event | Guard | To | Action | Limit |
|---|---|---|---|---|---|
| `BOOT` | `init_done` | no credentials | `PROVISIONING` | start SoftAP + portal | — |
| `BOOT` | `init_done` | credentials present | `CONNECTING` | start WiFi STA | — |
| `PROVISIONING` | `creds_saved` | — | `CONNECTING` | persist to NVS, stop AP | — |
| `PROVISIONING` | `portal_timeout` | credentials present | `CONNECTING` | stop AP | 300 s |
| `PROVISIONING` | `portal_timeout` | no credentials | `PROVISIONING` | remain; portal stays up | — |
| `PROVISIONING` | `btn_hold_5s` | — | *(ignored)* | already provisioning | — |
| `CONNECTING` | `wifi_up` | — | `LINKED` | start MQTT client | — |
| `CONNECTING` | `wifi_retry` | — | `CONNECTING` | back off, retry | cap 30 s |
| `CONNECTING` | `btn_hold_5s` | — | `PROVISIONING` | start SoftAP + portal | — |
| `LINKED` | `mqtt_up` | — | `OPERATIONAL` | publish discovery | — |
| `LINKED` | `wifi_down` | — | `CONNECTING` | stop MQTT client | — |
| `LINKED` | `btn_hold_5s` | — | `PROVISIONING` | stop STA, start AP | — |
| `OPERATIONAL` | `mqtt_down` | — | `LINKED` | stop publishing | — |
| `OPERATIONAL` | `wifi_down` | — | `CONNECTING` | stop MQTT client | — |
| `OPERATIONAL` | `ota_cmd` | valid URL | `UPDATING` | begin HTTPS download | — |
| `OPERATIONAL` | `btn_hold_5s` | — | `PROVISIONING` | stop STA, start AP | — |
| `UPDATING` | `ota_ok` | — | *(reboot)* | set boot slot, restart | — |
| `UPDATING` | `ota_fail` | — | `OPERATIONAL` | discard image, log | — |
| `UPDATING` | `wifi_down` | — | `CONNECTING` | abort download, discard | — |
| `UPDATING` | `btn_hold_5s` | — | *(ignored)* | never interrupt a flash write | — |
| *any* | `meter_burst` | — | *(no change)* | ingestion is state-independent | — |

Excluded by construction: `mqtt_up` outside `LINKED` (the client runs only in
`LINKED` and above); `creds_saved` outside `PROVISIONING` (the portal is the only
writer).

### 6.4 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 7. Meter Interface

### 7.1 Purpose & peer

Receives the E450's push telegrams and converts decoded DLMS values into this
project's measurement model. The peer is the meter, through an external M-Bus
level shifter. Traffic is one-way.

### 7.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-MTR-01** | Must | The device shall configure the meter UART as 2400 baud, 8 data bits, even parity, 1 stop bit. | Fresh boot; inspect the UART configuration | Configured 2400 8E1 | Any other framing | `[user]` interface spec §2.2 |
| **FR-MTR-02** | Must | The device shall invert the UART receive signal. | Drive the line with a known pattern | Pattern received intact | Bytes received with framing errors | `[user]` interface spec §2.2 |
| **FR-MTR-03** | Must | The device shall disable both internal pull-up and pull-down on the meter UART receive pin. | Inspect the pad configuration after boot | Both disabled | Either enabled | `[user]` interface spec §2.2 |
| **FR-MTR-04** | Must | The device shall never transmit on the meter link. | Monitor the meter link for 10 min in every state, including Provisioning and OTA | No transmitted edge observed | Any transmission, including during boot | `[user]` interface spec §1 |
| **FR-MTR-05** | Must | The meter serial length used for block detection shall be a configurable parameter, not a compiled-in constant. | Decode a capture whose serial is 8 chars, then one with 16 | Both decode with the parameter set accordingly | Rebuild required to change length | `[code]` published captures |
| **FR-MTR-06** | Must | The device shall recover frame alignment without external assistance when reception begins mid-burst, at a cost of no more than the cycle in progress. | Begin feeding from a byte offset inside a frame, then feed one complete cycle | Values decode from the partial cycle; the next complete cycle yields a full set including the identity | Recovery never happens, or every later cycle is also incomplete | `[derived]` interface spec §4.1 |
| **FR-MTR-07** | Must | The device shall discard any frame whose CRC does not validate. | Feed a capture with one corrupted CRC byte | That frame contributes nothing | Its values appear in the set | `[user]` interface spec §4.1 |
| **FR-MTR-08** | Must | The device shall not forward any part of a CRC-invalid frame to the decoder. | The same stimulus, observed at the decoder input | The decoder never receives those bytes | Any part of the frame forwarded | `[derived]` |
| **FR-MTR-09** | Must | The device shall map decoded OBIS codes to the measurement labels of Appendix B. | Feed a published capture | Every register maps to the right label, unit and scale | A value mapped to the wrong label | `[derived]` D-H2 |
| **FR-MTR-10** | Must | The device shall read the meter identity from the COSEM logical device name (`0.0.42.0.0.255`) or from device ID 1 (`0.0.96.1.0.255`), whichever the meter publishes. | Feed a capture carrying each object, then one carrying neither | Identity read from whichever is present; with neither, discovery defers | The wrong object preferred when both are present | `[code]` published captures |
| **NFR-MTR-01** | Must | The receive path shall buffer at least 512 bytes of a single burst without loss. | Feed a 491-byte telegram, the longest measured in the reference captures | No byte dropped | Any byte dropped | `[proposed]` interface spec §2.2 |
| **FR-MTR-11** | Must | The device shall store a DLMS key when supplied, and operate without one when it is absent. | Boot with a DLMS key stored, then with the slot empty | Both boot and decode an unencrypted telegram | An absent key blocks startup | `[user]` D-C1 |
| **FR-MTR-12** | Must | The device shall continue operating in every state when the meter produces no data. | Simulator `silence 60` | WiFi and the broker session hold; no measurement is published | A reset, or a stale set republished to fill the gap | `[derived]` |
| **FR-MTR-13** | Must | The device shall count discarded frames and make the count available on the serial console. | Simulator `fault fcs 2`; read the counter on the console | The count rises by exactly one per corrupted frame | The count is unavailable, or good frames are counted | `[derived]` |
| **FR-MTR-14** | Must | The device shall log a distinct condition when a burst is received but no block is found. | Simulator `fault noise 200`, then silence | A condition distinct from a decode failure is logged | Silence, or the same message a CRC failure produces | `[code]` published captures |

FR-MTR-04 is a safety property, not a convenience: the customer interface is a
metrologically sealed device, and transmitting on it is outside what a consumer
reader is permitted to do.

### 7.3 Protocol

M-Bus electrical layer (EN 13757-2) carrying DLMS/COSEM push telegrams in HDLC
framing, OBIS-addressed. Full detail in interface spec §4 and §5; not restated.

### 7.4 Failure modes

| Condition | Behaviour |
|---|---|
| No bytes received | No publication. The device remains in its network state and reports nothing about the meter |
| Continuous noise, no valid frame | Frames discarded; no publication (FR-AGG-06) |
| CRC failures | Frame discarded (FR-MTR-07); counted for diagnostics (FR-MTR-13) |
| Burst exceeds the buffer | Excess dropped, condition logged; the partial set is still decoded |
| Decoder finds no blocks | Distinct logged condition — the signature of a wrong serial length (FR-MTR-14) |

### 7.5 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml). Tier lives in
the test's own name; the plan references the IDs above and never restates them.

---

## 8. Home Assistant Publisher

### 8.1 Purpose & peer

Presents measurements to Home Assistant through MQTT discovery, so entities exist
without configuration on the Home Assistant side. The peer is the MQTT broker.

### 8.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-HA-01** | Must | The device shall publish a retained MQTT discovery configuration for each entity of Appendix B. | Fresh broker; connect the DUT; feed one cycle | One retained config per Appendix B entity present in the cycle | Configs for absent registers | `[user]` D-H1 |
| **FR-HA-02** | Must | The device shall derive each entity's `unique_id` from the meter serial. | Note `unique_id`s; reflash with a cleared NVS; reconnect | Identical `unique_id`s | IDs change across reflash | `[user]` D-H3 |
| **FR-HA-03** | Must | The device shall defer publishing discovery configurations until a meter serial has been decoded. | Connect with no meter data | No discovery topics | Discovery published with a placeholder serial | `[derived]` D-H3 |
| **FR-HA-04** | Must | The device shall publish energy entities with `device_class: energy` and `state_class: total_increasing`, and power entities with `device_class: power` and `state_class: measurement`. | Inspect discovery payloads in Home Assistant | Energy entities accepted by the Energy Dashboard | An energy entity rejected as unsuitable | `[user]` D-H2 |
| **FR-HA-05** | Must | The device shall publish only measurements present in the decoded cycle. | Feed a cycle missing `U2` | No `U2` in the payload | `U2` present as 0 or a stale value | `[user]` D-D3 |
| **FR-HA-06** | Must | The device shall register a last-will message marking itself unavailable, and publish availability on connection. | Cut DUT power while connected | Broker marks it unavailable within the keepalive window | Entity stays "available" indefinitely | `[pack:esp32]` |
| **FR-HA-07** | Must | The device shall not retain measurement state messages. | Subscribe fresh after the DUT is offline | No measurement state delivered | A retained stale reading delivered | `[derived]` |
| **FR-HA-08** | Must | The device shall derive its MQTT client identifier from its MAC address. | Read the client identifier the broker sees | It contains the device MAC | A random or fixed identifier that could collide between devices | `[user]` D-H3 |
| **NFR-HA-01** | Must | A measurement set shall be published within 2 s of the cycle boundary that produced it. | Timestamp cycle boundary and broker receipt | Delta ≤ 2 s | Delta exceeds 2 s under normal load | `[derived]` |

FR-HA-05 and FR-HA-07 are the same concern seen twice. A retained measurement, or
a zero standing in for an absent register, is indistinguishable from a real
reading of zero — and in an energy dashboard that difference is the entire point.

### 8.3 Topic layout

| Purpose | Topic | Retained |
|---|---|---|
| Discovery | `homeassistant/sensor/<serial>_<label>/config` | Yes |
| State | `gplug/<mac>/state` | No |
| Availability | `gplug/<mac>/status` | Yes |
| OTA command | `gplug/<mac>/cmd/ota` | No |

Discovery is keyed by meter serial so history survives replacing the hardware;
operational topics are keyed by MAC so two devices never collide (D-H3).

### 8.4 Failure modes

| Condition | Behaviour |
|---|---|
| Broker unreachable | No publication; sets discarded (FR-AGG-05); LWT marks unavailable |
| Broker returns after outage | New session, discovery republished, publishing resumes |
| Meter serial not yet known | No discovery, no state (FR-HA-03) |
| Credentials rejected | Retry as for any session failure; no fallback to Provisioning (FR-SUP-04) |

### 8.5 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 9. Provisioning Portal

### 9.1 Purpose & peer

Collects configuration once, from a phone browser, with no other input device
available. The peer is a human with a web browser.

### 9.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-PRV-01** | Must | In Provisioning the device shall operate a WPA2-protected SoftAP. | Enter Provisioning; attempt association with the derived passphrase, then a wrong one | Correct passphrase associates; wrong one rejected | An open network is offered | `[user]` D-C6 |
| **FR-PRV-02** | Must | The SoftAP passphrase shall be derived from the device MAC address by a documented rule. | Enter Provisioning; attempt association with the derived passphrase, then a wrong one | Correct passphrase associates; wrong one rejected | An open network is offered | `[user]` D-C6 |
| **FR-PRV-03** | Must | The device shall redirect DNS queries on the SoftAP to its own configuration page. | Associate and request any hostname | The configuration page is served | The request reaches the internet | `[pack:esp32]` |
| **FR-PRV-04** | Must | The device shall sample the button only after boot completes. | Hold the button through a reset | Device boots normally | Serial-download mode entered | `[derived]` interface spec §3 |
| **FR-PRV-05** | Must | The device shall persist submitted configuration to NVS before leaving Provisioning. | Submit configuration; power-cycle | Configuration survives | Values lost | `[derived]` D-C3 |
| **FR-PRV-06** | Must | The device shall retain previously stored configuration when Provisioning ends without a submission. | Enter Provisioning, wait for timeout without submitting | Prior configuration intact | Configuration erased on entry | `[derived]` |
| **FR-PRV-07** | Should | The configuration page shall list the WiFi networks the device can see. | Enter Provisioning with at least one access point in range | The configuration page lists it | An empty list while networks are in range | `[pack:esp32]` |
| **NFR-PRV-01** | Must | The SoftAP shall accept associations within 10 s of entering Provisioning. | Enter Provisioning, then associate with the SoftAP | Association succeeds within 10 s of entering the state | The SoftAP is advertised but refuses association | `[derived]` |
| **FR-PRV-08** | Must | The device shall resolve a broker address given as a `.local` hostname, or reject it at the portal with a stated reason. | Enter `homeassistant.local` as the broker address at the portal | Either it resolves and a session follows, or the portal states why it cannot | The portal accepts it and the device fails to resolve after the portal has closed | `[derived]` |

### 9.3 Configuration collected

See §17 for the full catalogue. The portal collects WiFi SSID and passphrase,
broker host and port, MQTT username and password, and an optional DLMS key.

### 9.4 Failure modes

| Condition | Behaviour |
|---|---|
| No client connects | Timeout returns to `CONNECTING` if credentials exist (FR-SUP-07); otherwise the portal stays up |
| Submission with an unreachable broker | Accepted and persisted. The portal cannot verify a broker it has no route to; the device retries after leaving Provisioning |
| Power lost mid-submission | NVS write is atomic; the prior configuration survives (FR-PRV-06) |

### 9.5 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 10. OTA Updater

### 10.1 Purpose & peer

Replaces firmware without physical access. The peer is a GitHub Release asset
served over HTTPS.

### 10.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-OTA-01** | Must | The device shall begin a firmware download on receipt of a URL on its OTA command topic. | Publish a valid URL to the command topic | Download begins within 5 s | Command ignored | `[user]` D-U2 |
| **FR-OTA-02** | Must | The device shall not initiate a firmware download from any other trigger. | Run 24 h with a newer release published | No download | Any unattended update | `[user]` D-U2 |
| **FR-OTA-03** | Must | The device shall verify the TLS certificate chain of the download server against an embedded certificate authority, and abort the download on failure. | Serve an image over TLS with an untrusted certificate | Download aborted, image not written | Image accepted | `[user]` D-U5 |
| **FR-OTA-04** | Must | The device shall write the downloaded image to the inactive OTA slot. | Trigger an update while running from one OTA slot | The image is written to the inactive slot | The running slot is overwritten | `[derived]` D-U3 |
| **FR-OTA-05** | Must | The device shall mark a newly booted image valid only after an MQTT session is established. | Update to an image that boots but cannot reach the broker | The image is never marked valid | Validation on a successful boot alone | `[user]` D-U4 |
| **FR-OTA-06** | Must | The device shall not require a meter decode before marking an image valid. | Update while the meter is silent | The image is marked valid once the broker session is established | A quiet meter causes a healthy build to roll back | `[user]` D-U4 |
| **FR-OTA-07** | Must | On reset before an image is marked valid, the bootloader shall revert to the previously valid image. | Update, then reset before validation completes | The previously valid image runs | The unvalidated image boots again | `[user]` D-U3 |
| **FR-OTA-08** | Must | The device shall continue decoding meter frames during a download. | Feed meter cycles throughout a download | Decoding continues | Ingestion stalls for the download | `[derived]` FR-SUP-08 |
| **FR-OTA-09** | Must | The device shall abort a download on loss of WiFi and discard the partial image. | Disable the access point mid-download | Download aborted, partial image discarded | Partial image marked bootable | `[derived]` |
| **NFR-OTA-01** | Must | The device shall report the running firmware version in its Home Assistant device information. | Read the device entry in Home Assistant after a session is established | The version shown matches the running firmware's version string | A blank, stale, or build-time-only version | `[user]` D-B5 |

FR-OTA-05 and FR-OTA-06 are stated separately because they fail separately. The
first is the bar; the second forbids raising it. An image that boots, joins WiFi
and reaches the broker is remotely recoverable, which is the only property that
matters for a device behind a cabinet door — and requiring meter traffic on top
would let a silent meter roll back a healthy build.

### 10.3 Failure modes

| Condition | Behaviour |
|---|---|
| URL unreachable | Download fails; device stays `OPERATIONAL` on the current image |
| Certificate invalid | Download aborted (FR-OTA-03); logged as a distinct condition |
| Image invalid or truncated | Rejected by the bootloader; previous image continues |
| Power lost mid-write | Inactive slot is incomplete; bootloader runs the valid image |
| New image cannot reach the broker | Never marked valid; next reset reverts (FR-OTA-07) |

### 10.4 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 11. Status Indicator

### 11.1 Purpose & peer

The only diagnostic channel available without disassembly, given that remote log
access is out of scope (§4.3). The peer is a person looking into the cabinet.

### 11.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-LED-01** | Must | The device shall show blue steady while in Provisioning. | Drive the DUT through every supervisor state | Indication matches §11.3 | Two states indistinguishable | `[derived]` D-L1 |
| **FR-LED-02** | Must | The device shall pulse green on each successful measurement publication. | Drive the DUT through every supervisor state | Indication matches §11.3 | Two states indistinguishable | `[derived]` D-L1 |
| **FR-LED-03** | Must | The device shall blink red while retrying a WiFi connection. | Drive the DUT through every supervisor state | Indication matches §11.3 | Two states indistinguishable | `[derived]` D-L1 |
| **FR-LED-04** | Must | The device shall show a distinct pattern throughout a firmware download and write. | Trigger an OTA; observe from command to reboot | Update pattern for the whole window | Indicator off at any point during the write | `[derived]` D-L1 |
| **FR-LED-05** | Must | The device shall leave all indicators off when connected and idle between cycles. | Drive the DUT through every supervisor state | Indication matches §11.3 | Two states indistinguishable | `[derived]` D-L1 |

FR-LED-04 exists to stop a person power-cycling a device mid-flash. It is the one
LED state whose absence has a physical consequence.

### 11.3 State mapping

| Supervisor state | Indication |
|---|---|
| `BOOT` | Red → green → blue, 500 ms each |
| `PROVISIONING` | Blue steady |
| `CONNECTING` | Red blink, 5 s period |
| `LINKED` | Red blink, 1 s period |
| `OPERATIONAL` | Off; green pulse 100 ms per publication |
| `UPDATING` | Alternating blue/green, 200 ms |

### 11.4 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 12. Network & Transport

### 12.1 Purpose

WiFi station mode, the MQTT client, the TLS stack, and the HTTP server are
ESP-IDF components this project configures and uses. None is tested in isolation;
all are exercised transitively by Parts A and B.

### 12.3 Dependency & lifecycle

Startup order is `NVS → WiFi → MQTT → publication`. Each stage gates the next;
none is retried independently of the supervisor state machine of §6.

---

## 13. Persistence

### 13.1 Purpose

NVS holds configuration across power cycles — which, given the device is powered
by the meter, happen whenever the meter does.

### 13.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-NVS-01** | Must | The device shall store configuration in NVS without encryption. | Provision the device, then dump the NVS partition | The stored configuration values are readable | Encryption present but undocumented — see NFR-SEC-01 | `[user]` D-C2 |
| **FR-NVS-02** | Must | The device shall operate with defaults and enter Provisioning when the NVS namespace is absent or unreadable. | Corrupt the NVS partition; boot | Device boots and enters Provisioning | Boot loop; crash | `[pack:esp32]` |
| **FR-NVS-03** | Must | Configuration shall survive an unclean power loss. | Cut power 100 times during normal operation | Configuration intact each time | Configuration lost or partially written | `[pack:esp32]` |

### 13.3 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 14. DLMS Decoder Library

### 14.1 Purpose & ownership

`esphome/dlms_parser` owns HDLC framing, CRC validation, transport auto-detection
and OBIS extraction. This project owns none of that logic and does not test it as
a project function — it is exercised transitively through §7.

### 14.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-DEC-03** | Must | The device shall apply scaling to decoded values exactly once. | Decode a capture with a known true value | The published value equals the meter's value in the Appendix B unit | A value out by any power of ten | `[code]` dlms_parser |
| **FR-DEC-04** | Must | Cumulative energy registers shall be carried as integers, not as `float`. | Decode a capture whose cumulative register exceeds 16,777,216 | The published value is exact | A value quantised by float | `[code]` dlms_parser |

FR-DEC-03 is stated because the failure it prevents is invisible: if the library
already scales and the application scales again, every reading is wrong by a
factor of a thousand and still looks like a plausible number.

`dlms_parser` does not scale behind the caller's back. It exposes the meter's
`scaler` alongside the raw bytes, and applies it only in the accessor
`value_as_float_with_scaler_applied()`. Reading that accessor and never scaling
again satisfies this requirement.

**FR-DEC-04 exists because that accessor returns `float`.** A 24-bit mantissa
represents every integer only up to 16,777,216, and a lifetime energy total in Wh
passes that at 16.8 MWh. A published capture already shows it: the meter sends
25,149,419 Wh and the accessor returns 25,149,420. One watt-hour is harmless in
itself, but Home Assistant derives consumption from differences between totals,
so a total that quantises produces phantom consumption and phantom zeroes. Read
`value` and `scaler` and keep cumulative registers in integer arithmetic; the
float accessor is fine for instantaneous power.

### 14.3 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 15. System Supervision

### 15.1 Purpose

Returns the device to service after a software hang without a person present.

### 15.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-WDT-01** | Must | A task that stops feeding the task watchdog shall cause a device reset within the configured watchdog period. | Stall one watchdog-subscribed task | The device resets within the configured task-watchdog period (5 s) | The device continues running with a hung task | `[pack:esp32]` |
| **FR-WDT-02** | Must | The device shall reset when a subscribed task fails to feed the watchdog within its timeout. | Command a test build into an infinite loop in one task | Device resets within the timeout and returns to `OPERATIONAL` | Device hangs indefinitely | `[pack:esp32]` |
| **FR-WDT-03** | Must | The watchdog shall not reset the device during a normal meter cycle gap or a firmware download. | Feed no meter data for 30 min while connected; separately, run a full OTA | No reset in either case | Any watchdog reset | `[pack:esp32]` |
| **FR-WDT-04** | Must | The device shall record the reset reason and make it available on the serial console after boot. | Force a watchdog reset; read the console | Reset reason reported as watchdog | Reason reported as power-on | `[pack:esp32]` |
| **FR-WDT-05** | Must | The device shall not reset as a response to any network, broker or meter fault. | Drop the access point, stop the broker, and silence the meter, each in turn | The reset reason is unchanged across all three | Any reset attributable to a peer fault | `[derived]` D-C4 |

FR-WDT-03 is the requirement that stops the watchdog becoming the fault. A meter
silent for minutes and a multi-minute OTA download are both normal, and a naive
timeout turns either into a reboot loop.

### 15.3 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 16. Device & Meter Identity

### 16.1 Purpose

Two identities exist and are deliberately not the same thing: the **meter**
identifies the data; the **device** identifies the hardware reading it.

### 16.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-ID-01** | Must | Home Assistant entity identity shall derive from the meter serial. | Publish discovery for a known meter, then present a different serial | `unique_id` follows the meter serial | Identity derived from the MAC, which loses history when the gPlug is replaced | `[user]` D-H3 |
| **FR-ID-02** | Must | MQTT client identity and operational topics shall derive from the device MAC address. | Inspect the client identifier and the operational topic prefix | Both contain the device MAC | Two devices colliding on one topic | `[user]` D-H3 |
| **FR-ID-03** | Must | Replacing the hardware reading a given meter shall not change entity identity. | Record entity IDs; swap in a second gPlug on the same meter | Entity IDs unchanged; history continues | Duplicate entities created | `[user]` D-H3 |

FR-ID-03 is the reason for the split. Energy statistics accumulate for years; a
device replaced after a failure must not restart that history.

### 16.3 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 17. Configuration Catalogue

| Key | Type | Default | Required | Set by |
|---|---|---|---|---|
| `wifi_ssid` | string | — | Yes | Portal |
| `wifi_pass` | string | — | Yes | Portal |
| `mqtt_host` | string | — | Yes | Portal |
| `mqtt_port` | uint16 | 1883 | No | Portal |
| `mqtt_user` | string | empty | No | Portal |
| `mqtt_pass` | string | empty | No | Portal |
| `dlms_key` | hex string | empty | No | Portal |
| `serial_len` | uint8 | 8 | No | Compile-time default, portal override |

---

## 18. Security

### 18.1 Threat profile

| Aspect | Position |
|---|---|
| Deployment | A private residence, inside a meter cabinet, on a home network |
| Attacker with physical access | Has already defeated every control here, and has better targets than a WiFi passphrase |
| Attacker on the local network | Can reach the broker and the device; cannot inject firmware without breaking TLS |
| Attacker in radio range during provisioning | Constrained to a few minutes, on a WPA2 link, requiring physical presence |
| Assets | WiFi and broker credentials; the ability to execute code on the device |
| Not an asset | Meter readings in transit on the local network. Consumption data is visible to anyone already on the LAN, which is accepted |

### 18.2 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-SEC-01** | Must | The device shall verify the TLS certificate chain for firmware downloads against an embedded certificate authority. | Serve an image with a self-signed certificate | Rejected, image not written | The image is accepted | `[user]` D-U5 |
| **FR-SEC-02** | Must | The device shall reject a firmware download whose certificate does not validate. | Serve an image with an expired certificate | Rejected, image not written | The image is accepted | `[user]` D-U5 |
| **FR-SEC-03** | Must | The provisioning access point shall require a WPA2 passphrase. | Scan for the SoftAP while Provisioning is active | The network advertises WPA2 | An open network is advertised | `[user]` D-C6 |
| **FR-SEC-04** | Must | The device shall not expose stored passphrases through any network interface. | Request every portal endpoint after provisioning | No passphrase in any response body | A passphrase echoed in a form field | `[derived]` |
| **NFR-SEC-01** | Must | Configuration is stored unencrypted; confidentiality against an attacker with physical flash access is **not claimed**. | Dump the flash of a provisioned device | The WiFi passphrase is readable in plaintext | Encryption present but undocumented | `[user]` D-C2 |

NFR-SEC-01 is deliberately a non-claim rather than a requirement. Writing
"credentials shall be protected at rest" when they are stored in plaintext would
produce an acceptance criterion that cannot fail — the defect §6.5.1 exists to
prevent. The accepted risk is recorded in §4. Its contract is inverted for the
same reason: it fails if someone quietly *adds* encryption, because that would
make the documented position false.

The asymmetry across these requirements is intentional. Plaintext NVS and a
short-lived provisioning window cost a credential in a scenario that requires
physical presence. A firmware download without certificate validation costs
arbitrary code execution to anyone on the path, permanently. Only the second
justifies its implementation cost.

### 18.3 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

### 18.2 Requirements

---

## 19. Degraded Operation

### 19.1 Degraded modes

| Fault | Retained capability | Lost capability |
|---|---|---|
| No WiFi | Decoding, LEDs, watchdog | Publication, OTA |
| WiFi but no broker | Decoding, LEDs, watchdog | Publication, OTA |
| No meter data | Network, OTA, portal | Measurements |
| Meter unpowered | None — the device is unpowered too | Everything |

---

## 20. Build, Versioning & Release

### 20.1 Requirements

| ID | Pri | Requirement | Stimulus | Expected | Must NOT | Provenance |
|---|---|---|---|---|---|---|
| **FR-BLD-01** | Must | The firmware version shall derive from the git tag. | Build at a tagged commit | The version string equals the tag | A version unrelated to the tag | `[user]` D-B5 |
| **FR-BLD-02** | Must | An untagged build's version shall include the short commit hash. | Build an untagged commit | The version string includes the short commit hash | A version indistinguishable from a tagged release | `[user]` D-B5 |
| **FR-BLD-03** | Must | The simulated build shall be selected at compile time. | Inspect the production binary and the variant-selection mechanism | Selection is a compile-time option; the production binary contains no capture data | Any runtime path that enables simulated data | `[user]` D-T2/T3 |
| **FR-BLD-04** | Must | The production build shall contain no capability to generate measurement data. | Search the production binary for the embedded capture data | Absent | Any capture data present | `[user]` D-T3 |
| **FR-BLD-05** | Must | The simulated build's version string shall carry a `-sim` suffix. | Build the simulated variant | The version string carries the `-sim` suffix | A simulated build indistinguishable from production | `[user]` D-T4 |
| **FR-BLD-06** | Must | The simulated build shall publish under a discovery prefix distinct from the production build. | Run the simulated build against a Home Assistant instance | Entities appear only under the test prefix | Production entities created or overwritten | `[user]` D-T4 |
| **FR-BLD-07** | Must | Every push shall build the firmware and run the host tests. | Inspect `.github/workflows/` | Both workflows trigger on `push`; the host job runs `ctest` | A workflow that runs only on a tag, or a build that skips the tests | `[user]` D-B1 |
| **FR-BLD-08** | Must | Firmware shall be published only from a tagged commit. | Push to a branch without a tag | No release asset produced | A downloadable binary published | `[user]` D-B2 |

FR-BLD-04 and FR-BLD-06 protect the same thing from two directions. Home
Assistant's `total_increasing` statistics are effectively permanent — one
synthetic energy total contaminates a long-term series that cannot be cleanly
repaired. So the production binary must be incapable of fabricating readings, and
the simulated binary must be incapable of reaching production entities.

### 20.2 Release gate

Bench tests require the Embedded Workbench, which a hosted CI runner cannot
reach. **Tagging is therefore the human assertion that the bench suite passed**
(D-B3). This is a deliberate position, not a missing gate; automating it would
require a self-hosted runner on the bench network.

### 20.3 Tests

Which tests cover these requirements, and what each produced, is in
[`testing/test-plan.yaml`](../../testing/test-plan.yaml).

## 21. Operational Procedures

A reading path through the chapters above; component detail is not restated.

| Stage | Path |
|---|---|
| **First flash** | Partition layout Appendix C · flash over USB |
| **Provision** | §9 portal · §17 configuration catalogue · §11 blue steady confirms |
| **Verify** | §8 discovery topics appear · Energy Dashboard accepts the entities (§8.5) |
| **Operate** | §11 indicator states · §19.1 degraded modes |
| **Reconfigure** | §6 button hold 5 s → §9 portal → returns to §6 `CONNECTING` |
| **Update** | §10 publish URL to the command topic · §11 update pattern · §10.4 verify version |
| **Recover** | WiFi lost: §6 retries, no action needed · bad firmware: §10 reset reverts · hang: §15 watchdog · configuration lost: §9 portal |

Nothing in this table requires opening the meter cabinet except the first flash.
That is the design intent of §4.1's risk table, stated as a procedure so a
deviation is visible.

## 22. Verification & Validation

### 22.0 Test architecture

| Tier | Runs on | Speed | Catches |
|---|---|---|---|
| **host** | Dev machine, no hardware | ms, every push | Decode, mapping, scaling, cycle-boundary arithmetic |
| **target** | The ESP32-C3 alone | seconds | UART configuration, NVS, GPIO, boot behaviour |
| **bench** | Device on the Embedded Workbench with AP, broker and OTA relay | minutes | Provisioning, sessions, discovery, OTA, rollback, watchdog, resilience |
| **field** | Installed at the meter | once | Physical layer, real frames, meter power, WiFi coverage |
| **other** | CI or review | — | Artefact properties not observable at runtime |

Layer mapping: **L0** is exercised transitively and has no host-tier tests of its
own — empty cells there are expected, not gaps. **L1** interfaces are split, with
their pure cores at host and their wire behaviour at target or bench. **L2**
application logic is host-tier throughout.

**bench and field differ by control, not by realism.** On the bench the peers are
ours, so faults can be injected — the broker is stopped, the access point is
switched off, an untrusted certificate is served. In the field the peers are the
real ones and can only be observed; the meter cannot be asked for a malformed
frame, and the basement cannot be asked for worse coverage.

**An M-Bus simulator now drives the bench, so the field tier is small** — see
[`decisions.md`](../decisions.md) T7. Decode and protocol cases sit at bench,
where the frames can be chosen and malformed ones injected. The field tier keeps
only what a simulator cannot stand in for: the real meter's own values (AC-6) and
24 h on meter power with cabinet WiFi (AC-7).

A green bench run does not prove either of those. The simulator emits bytes we
authored, so it can confirm the device reads what it is given and can never
confirm the meter gives what we assumed.

`other` in the table above is **not a tier.** No execution environment observes
those properties; they are checked by CI or review, and only FR-BLD-04 and
FR-BLD-08 carry it.

Which tests cover which requirement, at which tier, with what equipment and what
each produced, is in [`testing/test-plan.yaml`](../../testing/test-plan.yaml).
Nothing about coverage is maintained here — a hand-kept column is stale the
moment a test changes.

### 22.1 Acceptance scenarios

| ID | Scenario | Tier |
|---|---|---|
| **AC-1** | A fresh device is provisioned from a phone and its entities appear in Home Assistant with no configuration there | bench |
| **AC-2** | Energy entities are accepted by the Energy Dashboard and accumulate correctly across a simulated day | bench |
| **AC-3** | A 10-minute access-point outage is survived with no intervention and no AP-mode fallback | bench |
| **AC-4** | A firmware update is delivered, applied and confirmed entirely over the network | bench |
| **AC-5** | A firmware that cannot reach the broker is reverted automatically | bench |
| **AC-6** | The real meter's frames decode with the values matching the meter's own display | field |
| **AC-7** | The device operates for 24 h on meter power with WiFi coverage sufficient for continuous publication | field |

AC-6 is the only scenario that can invalidate Phase 1's conclusions, and AC-7 is
the only one that can invalidate the deployment. Both are field-tier by
necessity, and both are why Phase 3 exists as a phase rather than a formality.

### 22.2 Traceability

Traceability is the join between the stable IDs in this document and the tests
in [`testing/test-plan.yaml`](../../testing/test-plan.yaml), which reference them
and never restate them.

A **test** is `not done`, `successful` or `failed`, and those fields are written
by whatever ran it, never by hand. `not done` carries its reason, and the two
reasons are different in kind: *not written yet* is a backlog item, *needs a
capability that is unavailable* is not, and no amount of writing fixes it.

A **requirement** is met when every test that verifies it is successful —
including the check that its *must not* did not happen. A passing test that never
looked for the prohibited outcome establishes less than it appears to.

At revision 0.2.0: **86 requirements, 59 carrying a contract, 10 tests
implemented and passing, all at host tier.** No target or bench test exists yet,
so nothing that depends on a wire or a peer is verified — including the meter
interface, which is where the device is currently known to be failing.

---

## Appendix A — Constants

| Constant | Value | Source |
|---|---|---|
| Meter UART | 2400 8E1, inverted RX, GPIO7 | Interface spec §2.2 |
| Cycle gap threshold | 2000 ms | Interface spec §4.2 |
| Cycle buffer | 2048 bytes | Interface spec §4.2 |
| Reconnect backoff cap | 30 s | FR-SUP-05 |
| Portal timeout | 300 s | FR-SUP-07 |
| Button hold | 5000 ms | FR-SUP-06 |
| Publish latency budget | 2 s | NFR-HA-01 |
| MQTT reconnect budget | 60 s | NFR-SUP-01 |

## Appendix B — Published entities

| Label | OBIS | Quantity | Unit | `device_class` | `state_class` | Category |
|---|---|---|---|---|---|---|
| `Ei` | 1.8.0 | Energy imported | kWh | `energy` | `total_increasing` | primary |
| `Eo` | 2.8.0 | Energy exported | kWh | `energy` | `total_increasing` | primary |
| `Pi` | 1.7.0 | Power imported | kW | `power` | `measurement` | primary |
| `Po` | 2.7.0 | Power exported | kW | `power` | `measurement` | primary |
| `U1` `U2` `U3` | 32/52/72.7.0 | Voltage L1–L3 | V | `voltage` | `measurement` | diagnostic |
| `I1` `I2` `I3` | 31/51/71.7.0 | Current L1–L3 | A | `current` | `measurement` | diagnostic |

Registers decoded but not promoted to entities — reactive energy and per-tariff
and per-phase values — remain in the state payload. Promoting one later is a
discovery change, not a decode change.

## Appendix C — Partition layout (4 MB)

| Name | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data | 0x9000 | 24 K |
| `otadata` | data | 0xF000 | 8 K |
| `phy_init` | data | 0x11000 | 4 K |
| `ota_0` | app | 0x20000 | 1920 K |
| `ota_1` | app | 0x200000 | 1920 K |

Two application slots are required by FR-OTA-04. **This layout cannot change
after the device is installed** without USB access, so it is fixed at the first
flash.

## Appendix D — MQTT topics

| Purpose | Topic | Retained | QoS |
|---|---|---|---|
| Discovery | `homeassistant/sensor/<serial>_<label>/config` | Yes | 1 |
| State | `gplug/<mac>/state` | No | 0 |
| Availability | `gplug/<mac>/status` | Yes | 1 |
| OTA command | `gplug/<mac>/cmd/ota` | No | 1 |

## Related

- [[MBUS-E450-Interface-Spec]] — meter and board interface facts
- [[decisions]] — settled design decisions with provenance
