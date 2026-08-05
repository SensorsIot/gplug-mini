# gPlug-mini — Design Decisions

Settled decisions for the gPlug-mini smart meter reader, from the design
interview. The product is **gPlug-mini**; its repository is `gplug-mini`. This is the input to the FSD: everything here is decided and must not
be re-asked. What is genuinely undecided is in §4, and alternatives that were
considered and rejected are in §5 so they are not helpfully reintroduced later.

**Provenance** is marked on every decision. `[user]` was answered directly.
`[derived]` was recommended and accepted without objection — still binding, but
the weaker of the two, and worth a second look if it later causes friction.

Hardware and protocol facts live in
[`MBUS-E450-Interface-Spec.md`](Functionality/MBUS-E450-Interface-Spec.md) and are not repeated
here.

---

## 1. Product

| # | Decision | Gates |
| --- | --- | --- |
| P1 | Read a Landis+Gyr E450 over its customer information interface and publish to Home Assistant over MQTT. `[user]` | Everything |
| P2 | Completely independent of any predecessor project — no code, no documents, no clause IDs. `[user]` | Repo contents, the interface spec's scope |
| P3 | Deployed in a basement meter cabinet, powered from the meter's 5 V rail. Every design choice is judged by "does this require a physical visit?" `[user]` | Portal entry, OTA, failure behaviour |
| P4 | ESP-IDF **6.0.2** — newest stable. Not Arduino, not PlatformIO, not ESPHome. `[user]` | Toolchain, CI image, component sources |

## 2. Decisions

### 2.1 Decoding

| # | Decision | Gates |
| --- | --- | --- |
| D1 | Depend on `esphome/dlms_parser^1.2.0` (Apache-2.0) from the Espressif Component Registry rather than writing a decoder. `[user]` | `idf_component.yml`; the decoder is off the critical path |
| D2 | The application compiles as **C++** — the library is C++20. `[derived]` | Project structure, build config |
| D3 | Publish only the values present in the decoded burst. A zero meaning "absent" is indistinguishable from a real zero reading. `[derived]` | Payload construction |

### 2.2 Home Assistant

| # | Decision | Gates |
| --- | --- | --- |
| H1 | **MQTT Discovery** — the device publishes retained `homeassistant/…/config`. No YAML in HA, because the DSO can change the pushed register set without notice. `[derived]` | Startup sequence, topic layout |
| H2 | **Energy Dashboard set**: `Ei`/`Eo` as `device_class: energy` + `state_class: total_increasing`; `Pi`/`Po` as `power` + `measurement`; `U1..U3`/`I1..I3` as diagnostic. Remaining registers stay in the JSON payload, unpromoted. `[user]` | Entity model, discovery payloads |
| H3 | Entity `unique_id` derives from the **meter serial**, so energy history survives replacing the gPlug. MAC identifies the *device*: MQTT client ID and topic prefix. `[user]` | Startup ordering — discovery cannot be published until the first successful decode |
| H4 | **No buffering** when the broker is unreachable: drop, reconnect, carry on. `total_increasing` lets HA reconstruct consumption from the next counter value, so a gap costs resolution and not correctness. `[derived]` | No queue, no flash wear, no replay ordering |

### 2.3 Configuration and provisioning

| # | Decision | Gates |
| --- | --- | --- |
| C1 | Configurable: WiFi SSID/password, broker host/port, MQTT username/password, and an empty DLMS key slot. Nothing else — the discovery prefix and device name have correct defaults. `[derived]` | Portal form, NVS schema |
| C2 | **Plain NVS**, no flash encryption. A deliberate, documented position: flash encryption needs irreversible eFuse burning, and anyone with physical access to the meter cabinet has better options than the WiFi password. `[user]` | Security section of the FSD |
| C3 | AP mode **only** on first boot with no stored credentials, or on a deliberate ~5 s button hold with a few-minute timeout back to retrying. `[user]` | State machine |
| C4 | **Never self-demote to AP on WiFi loss.** Retry forever, backoff capped ~30 s. A router reboot must not strand the device in a portal nobody can see. `[user]` | The single most consequential availability decision |
| C5 | GPIO9 is sampled only **after** boot — it is the ESP32-C3 strapping pin, and holding it low through reset forces serial-download mode. `[derived]` | Button handling |
| C6 | The provisioning AP is **WPA2**, password derived per-device from the MAC and documented so it is recoverable without a label. `[user]` | Portal, user manual |

### 2.4 Update

| # | Decision | Gates |
| --- | --- | --- |
| U1 | OTA **pulls** over HTTPS from a GitHub Release asset. Nothing has to reach into the LAN, and it works behind NAT. `[derived]` | CI publishing, release asset naming |
| U2 | Triggered by an **MQTT command** carrying a URL. **No auto-polling** — unattended self-update delivers a bad build everywhere before anyone notices. `[user]` | Command topic |
| U3 | Two OTA app partitions plus `otadata`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. `[derived]` | Partition table — fixed before the first flash |
| U4 | Firmware marks itself valid **when MQTT connects**. That proves WiFi, DHCP, DNS, broker reachability and credentials — exactly what must work to send the next OTA. Deliberately does *not* require a meter decode, so a quiet meter cannot roll back a healthy build. `[user]` | Rollback semantics |
| U5 | **Strict HTTPS certificate verification** with an embedded CA. The one channel where an attacker gets arbitrary code execution, so it gets the effort the other two do not. `[user]` | TLS config, cert bundle |

### 2.5 Verification

| # | Decision | Gates |
| --- | --- | --- |
| T1 | Four tiers, cost-ordered. **Host** (CI, no hardware): the decoder against published E450 captures. **Target** (the ESP32-C3 alone): UART configuration, NVS, GPIO, boot behaviour. **Bench** (Embedded Workbench): provisioning, HA topics, OTA, rollback, WiFi resilience, and the decode-to-publish path. **Field** (the cabin, on site): physical layer, real frames, meter power, and WiFi coverage in the cabinet. A fifth label, `other`, is **not a tier** — it is the bucket for properties no execution environment can observe, checked by CI or review (FR-BLD-04, FR-BLD-08). `[user]` | Test layout, CI scope, the `tier` field of every test spec |
| T2 | A **compile-time** sim build (Kconfig) injects capture bytes at the UART boundary, preserving inter-chunk timing so the 2000 ms gap logic is genuinely exercised. Everything above that boundary is the identical code path. `[user]` | Build variants |
| T3 | Compile-time and not runtime, so a shipped binary *cannot* fabricate readings. One synthetic value in HA's `total_increasing` statistics is effectively permanent. `[derived]` | Kconfig, artifact list |
| T4 | The sim build uses a distinct discovery prefix, a marked `unique_id`, and a `-sim` version suffix, so a bench session leaves no debris in a production HA. `[derived]` | Sim build config |
| T5 | ~~No hardware meter simulator. The decoder is the same code whether bytes arrive from a file or a wire; what the wire adds is the physical layer, which is what the field tier confirms.~~ **Superseded by T7.** Kept because test results recorded before 2026-08-05 were designed under it. `[user]` | Scope |
| T6 | After flashing the production build on the bench, confirm it boots, connects WiFi and MQTT, and reports no meter data — closing the "we tested the sim binary and shipped a different one" gap. `[derived]` | Bench checklist |
| T7 | **An M-Bus simulator exists and is the bench's meter.** T5's premise — that the wire adds only the physical layer — was wrong: it also adds burst timing and the receive path, which no file-fed test reaches. The simulator found a receive defect the host tier cannot see (the first ~24 bytes of every burst lost) on its first day. Decode and protocol cases therefore move from field to bench; the field tier keeps only what stays irreproducible — the real meter's own values (AC-6) and 24 h on meter power with cabinet WiFi (AC-7). `[user]` | Tier of every meter-facing case, field-tier scope, T5 |

### 2.6 Build and release

| # | Decision | Gates |
| --- | --- | --- |
| B1 | Every push: host tests plus the **sim** firmware. Publishes nothing. `[user]` | Workflow triggers |
| B2 | A tag: build and publish the **production** firmware to a Release. `[user]` | Release job |
| B3 | **The tag is the gate.** Bench tests need the workbench Pi, which a GitHub-hosted runner cannot reach, so tagging is the human assertion that the bench suite passed. A deliberate choice, not a missing gate. `[user]` | CI honesty; the `setup-action` skill will otherwise flag this |
| B4 | Only needed artifacts. No merged factory image — the workbench flashes from parts at offsets. `[user]` | Artifact list |
| B5 | Versions derive from the git tag via `esp_app_desc_t`, short SHA appended when untagged. One string reaches the HA device page, the OTA log and the release asset, so they cannot disagree. `[user]` | Versioning, release naming |

### 2.7 Indication

| # | Decision | Gates |
| --- | --- | --- |
| L1 | Blue solid in portal mode; green pulse on publish; red blink while retrying WiFi; off when idle and healthy; a distinct pattern during OTA so nobody power-cycles a device mid-flash. `[derived]` | LED module |
| L2 | LED polarity behind a single constant — the board is active-high today, and a revision that inverts it should be a one-line change. `[derived]` | LED module |

---

## 3. Resolved by measurement

All four were properties of the hardware or of `dlms_parser`, and all four are
now measured rather than assumed.

| Question | Answer | Evidence |
| --- | --- | --- |
| Is the meter serial 8 or 16 characters? | **Both.** One capture carries `LGZ1030655933512`, the other `44337811`, from the same meter model | Published captures; the length is a parameter (FR-MTR-05), defaulting to 8 |
| Does the library already scale values? | **No.** It exposes the meter's `scaler` beside the raw bytes and applies it only in `value_as_float_with_scaler_applied()` | Library source and its output on the captures |
| Does it assemble bursts across frames? | **Yes**, and it must: the meter sends DLMS General Block Transfer and the 8-character serial straddles a block boundary — `4433` ends one block, `7811` begins the next | Block flags `0x40` more / `0xC0` last, confirmed against the library's own fixtures |
| Flash size — 4 MB assumed | **4 MB confirmed**, embedded XMC, JEDEC 20/4016 | `POST /api/chip/info` on the device. ESP-IDF defaults this target to 2 MB, so the build had been addressing half the flash |

**One answer changed the shape of the work.** Because the library reassembles,
the 2000 ms gap is a publish-cadence rule and not a decode trigger, and the
last-block flag — not silence — is the authority on when a transmission ends.

**One finding was not on the list.** `value_as_float_with_scaler_applied()`
returns `float`, which is exact only to 16,777,216; a lifetime energy total in Wh
passes that at 16.8 MWh. A published capture already shows the meter sending
25,149,419 Wh and the accessor returning 25,149,420. Home Assistant derives
consumption from differences between totals, so a quantised total invents
consumption that never happened — hence FR-DEC-04, cumulative registers stay
integers. Looking for one bug is how the other was found.

## 4. Open — unknowable until the field test

- **Does WiFi reach the cabinet?** A basement plus a metal enclosure. This has
  sunk more of these projects than any protocol bug, which is why it is an
  explicit test rather than an assumption.

---

## 5. Considered and rejected

Recorded so they are not helpfully reintroduced.

| Rejected | Because |
| --- | --- |
| Writing our own DLMS/HDLC decoder | The meter is unreachable, so a hand-written decoder would be unverifiable until deployment day. The library is Apache-2.0, ESP-IDF-native, and already handles encryption and segmentation |
| Gurux DLMS for ANSI C | Built around a client that establishes an association and requests objects. This interface only pushes, so most of the library is unreachable and its framing is entangled with request/response |
| Falling back to AP mode when WiFi drops | Protects against a failure that happens once, at setup, while creating one that recurs forever, unattended, in a basement — see C4 |
| Auto-polling for new releases | Delivers a bad build to every device before anyone notices |
| ~~A second ESP32 as a hardware meter simulator~~ **— no longer rejected; built, see T7** | The stated reason was "apparatus built to test a path the host tier already covers". It does not: the host tier feeds the decoder from a file, which exercises neither burst timing nor the UART receive path. The simulator's first day exposed a defect in exactly that gap |
| Flash encryption / encrypted NVS | Irreversible eFuse burning and materially harder development flashing, to protect a WiFi password from someone already inside the meter cabinet |
| An open provisioning AP | Rejected in favour of WPA2 — see C6 |
| A merged factory image artifact | The workbench flashes from parts at offsets; nothing would consume it |
| Buffering readings across a broker outage | `total_increasing` makes gaps cost resolution rather than correctness — see H4 |
| A self-hosted CI runner to gate on bench tests | More machinery than this project warrants. The tag carries that meaning instead — see B3 |
