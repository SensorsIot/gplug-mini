# gPlug ↔ Landis+Gyr E450 — M-Bus / DLMS Interface Specification

The physical, link and application layers between a Landis+Gyr E450 smart meter
and the gPlug (ESP32-C3) reader, plus the gPlug pin assignment.

This document describes **the meter and the board** — what any firmware must
respect in order to read this meter. It does not describe firmware. Where a
number is an implementation choice rather than a property of the interface, it is
marked as such.

## 1. Scope

The E450 exposes a **customer information interface (CII)** — an M-Bus
(EN 13757-2) *electrical* interface carrying **DLMS/COSEM push telegrams** in
**HDLC framing**, with values addressed by **OBIS codes** (IEC 62056-6-1).

Only the electrical layer is M-Bus. The application layer is DLMS, not
EN 13757-3 — there are no `68 L L 68` frames and no DIF/VIF data records.

The gPlug is a **passive listener**: it never transmits on the meter link. The
meter pushes unsolicited bursts on a timer; the reader receives, validates and
decodes them. There is no addressing, no polling, no collision handling, and no
master role to implement.

```
Landis+Gyr E450 ──M-Bus──► level shifter ──inverted TTL UART──► ESP32-C3 GPIO7
   (push telegrams,          (external            2400 8E1
    DLMS/HDLC)                front-end)
```

---

## 2. Physical Layer

### 2.1 Meter-side connector → RJ45

Mapping of the E450 customer-interface connector to the RJ45 on the gPlug cable:

| Meter pin | Signal | RJ45 pin |
| --------- | ------------ | -------- |
| 1 | 5 V | 2 |
| 2 | Data Request | 3 |
| 3 | Data GND | 4 |
| 4 | N.C. | 5 |
| 5 | Data | 6 |
| 6 | Power GND | 7 |

**Data** (meter pin 5 / RJ45 pin 6) is the only signal firmware reads.
**Data Request** is a meter-side enable asserted by the wiring and front-end, not
driven by firmware. The 5 V rail powers the gPlug from the meter — the device has
no independent supply, so it is unpowered whenever the meter is.

### 2.2 UART parameters

| Parameter | Value |
| --------- | ----- |
| Baud rate | **2400** |
| Data bits | **8** |
| Parity | **Even** |
| Stop bits | **1** |
| Flow control | none |
| RX signal | **inverted** |

Shorthand: **2400 8E1, inverted RX**.

Inversion is mandatory — it compensates for the external M-Bus level shifter,
which presents an idle state opposite to what a UART expects. On the ESP32-C3
this is a peripheral setting, not a wiring change. The RX pin must also have both
internal pull-up and pull-down disabled, since either will fight the front-end's
output stage.

At 2400 baud with 11 bits per character (start + 8 data + parity + stop), the
line carries ~218 bytes/s.

Frames are far shorter than the 512-byte protocol ceiling, and not by accident:
the meter's **HDLC transmit buffer is 128 bytes** (§7.6), so it fragments every
telegram to fit. Measured frames run 63–141 bytes on the wire, about 0.3–0.65 s
each, and a complete telegram of 3–4 frames is 338 bytes (1.55 s) and 491 bytes
(2.25 s) in the two reference captures.

Two consequences for a reader. A receive buffer must hold a whole **telegram**,
not a frame — no single frame is a complete reading. And a telegram occupies a
third to a half of the 5 s push interval, so the line is busy far more of the
time than the frame sizes suggest.

---

## 3. gPlug Board (ESP32-C3)

| Function | GPIO | Direction / polarity |
| -------- | ---- | -------------------- |
| **M-Bus data (UART RX)** | **7** | input, inverted, no pull |
| UART TX | 21 | output — unused; the meter link is receive-only |
| LED red | 1 | output, active high |
| LED blue | 3 | output, active high |
| LED green | 4 | output, active high |
| Button | 9 | input, active low, internal pull-up |

Constraints that follow from the silicon and the board, which firmware cannot
choose differently:

- **GPIO9 is the ESP32-C3 boot strapping pin.** Held low at reset it forces
  serial-download mode, so it cannot be used as a reset-time input and nothing
  external may hold it low during boot.
- **GPIO20/21 are the default console UART.** Using GPIO21, and keeping a second
  log stream off the meter UART, requires the secondary console to be disabled in
  the build configuration.
- **LED colour assignment is a firmware decision**, not an interface fact. The
  board provides three independently driven active-high LEDs. Keeping the
  polarity behind a single constant makes a board revision with active-low LEDs a
  one-line change.

---

## 4. Link Layer — HDLC Framing

Frame layout (HDLC type 3, no segmentation):

```
7E | A0 LL | dest dest | src | ctrl | HCS(2) | information … | FCS(2) | 7E
 0    1  2    3    4      5     6      7  8     9 … len-2      len-1 len   len+1
```

| Element | Value / rule |
| ------- | ------------ |
| Flag | `0x7E` opens and closes every frame |
| Format byte | high nibble `0xA0`; low 3 bits are the high bits of the length |
| Frame length | `((format & 0x07) << 8) \| length_byte`, counting **from the format byte through the FCS** — so the closing flag sits at `frame_start + 1 + length` |
| Addresses | HDLC variable-length: a byte with bit 0 clear continues the address. This meter sends **destination `CE FF` (two bytes)** and **source `03`** (one), putting **control at offset 6** and the HCS at 7–8 |
| HCS | same CRC as the FCS, over offsets `1 … 6` — the format byte through the control byte — little-endian at offsets 7–8 |
| Payload window | bytes `[9 … length-1)` — after the HCS, before the FCS and the closing flag |
| FCS | CRC-16/CCITT, polynomial **0x8408** reflected, init `0xFFFF`, final XOR `0xFFFF` (CRC-16/X-25) |
| FCS coverage | from the format byte (offset 1) through the last information byte, i.e. offsets `1 … length-2`, little-endian at `length-1` and `length` |

> **The two-byte destination is measured, not assumed.** Every offset above was
> verified against `e450_serial8.hex` and `e450_serial16.hex`, which agree on all
> of them, and the HCS only validates on this reading. A parser that assumes
> single-byte addresses takes the control byte for an address, reads the
> information field one byte early, and — because it never checks the HCS —
> discovers none of it.

Frames from this meter fit within **512 bytes**. A declared length outside
5 … 510 means the parser is desynchronised, not that a long frame arrived.

**The information field is not the application data.** It carries, in order: the
DLMS **LLC header `E6 E7 00`** — present in the *first frame of a telegram only*,
not in every frame — followed by a **General Block Transfer** element. Only after
reassembling those blocks (§4.2) does a DLMS APDU appear.

### 4.1 Resynchronisation

The link is a bare serial line with no framing below HDLC, and the reader is
powered by the meter, so it will routinely start listening mid-burst. Recovering
alignment without help from the sender is therefore a normal operating mode, not
an error path:

- Any byte that is not `0xA*` immediately after a flag means the flag was data,
  not a frame start. Discard and resume hunting.
- A second `0x7E` while still reading the format byte means the first was a
  closing flag. Restart the frame from that point.
- A frame whose CRC fails is **discarded entirely**. Partial or repaired frames
  must never reach the decoder — once decoded, a corrupted OBIS value is
  indistinguishable from a real one and would be published to the consumer as
  fact.

### 4.2 Burst and cycle model

The E450 pushes a **burst of several frames** per transmission cycle, then goes
quiet until the next cycle. A single frame is not a complete reading — but the
reason is more specific than "the data is spread out": **one DLMS APDU is
fragmented across the frames by General Block Transfer**, because the meter's
HDLC transmit buffer is only 128 bytes (§7.6).

Each frame's information field carries a GBT element:

```
E0 | block-control | block-number(2) | acknowledged-block(2) | length(BER) | payload …
```

with block control `0x40` in streaming mode, `0xC0` on the **last** block, and
block numbers counting from 1. Concatenating the payloads in block order
reconstructs the APDU.

Consequences for any reader:

1. Payloads of CRC-valid frames must be **accumulated across the burst** before
   decoding is attempted. Half an APDU decodes to nothing useful, and — worse —
   a value split across a block boundary reads as a *shorter, valid-looking*
   value if reassembly is skipped. `e450_serial8.hex` is the fixture for exactly
   this: the meter serial `44337811` is split `4433` / `7811` across a block
   boundary, and a frame-at-a-time decoder reads a truncated serial without any
   error.
2. **The end of a telegram is marked by the GBT last-block flag `0xC0`, not by
   silence.** Silence is what separates one *push list* from the next: this meter
   is configured with four of them (§6.1) on four schedules, so several telegrams
   can arrive back to back in one cycle. A gap of more than ~2000 ms between
   frames marks the end of a cycle — well clear of intra-telegram frame spacing
   and far below the shortest push interval (5 s). The exact value is tunable.
3. The accumulation buffer is bounded by the longest cycle; 2048 bytes has proven
   sufficient. This is an implementation choice.

Reader behaviour that follows: reassemble on the GBT flags, publish on the
silence.

### 4.3 Encryption

The DLMS application layer supports encryption (AES-128-GCM, 16-byte key),
enabled and keyed by the DSO. **This installation pushes plaintext telegrams** —
decoding has been demonstrated against it with no key.

**Landis+Gyr's own guidance is that it should be on.** The public *Endkunden-
schnittstelle für die Schweiz — Standard CIP-Liste* (16 Sep 2024, §2.3) states
that for the meter to satisfy the METAS Art. 8b data-security check and the
privacy directive, *"muss die Verschlüsselung der CIP-Daten zwingend
eingeschaltet werden"* — encryption must be enabled — and its reference
configuration ships with the CII message security policy set to
**Authenticated + Encrypted**, security suite (0) AES-GCM-128.

That this installation nonetheless pushes plaintext is therefore a **deviation
by the DSO from the vendor recommendation**, not the default. It can be changed
without notice and without any change visible to the reader beforehand.

**Whether the CII is encrypted is a DSO policy decision, and it splits by
country.** Swiss deployments are plaintext: the EKZ-funded reference
implementation (§8) carries E450 captures explicitly labelled unencrypted, and
its parser accepts both. Austrian deployments are routinely encrypted and require
a Global Unicast Encryption Key obtained from the network operator; published
accounts of key-protected E450 CIIs come from Wiener Netze and Klagenfurt.

The practical consequence is that guidance found online about the E450 needing a
key does not necessarily apply here, and neither does the reverse. Provisioning a
key slot in configuration costs little and leaves the door open; implementing the
cipher before there is a key to test against does not.

---

## 5. Application Layer — DLMS / OBIS Decoding

### 5.1 DLMS data type tags

| Tag | Type | Bytes consumed |
| --- | ---- | -------------- |
| `0x0F` | int8 | 2 |
| `0x11` | uint8 | 2 |
| `0x10` | int16 (big-endian) | 3 |
| `0x12` | uint16 (big-endian) | 3 |
| `0x05` | int32 (big-endian) | 5 |
| `0x06` | uint32 (big-endian) | 5 |
| `0x14` / `0x15` | int64 / uint64 | 9 |

Also present in the stream but carrying no numeric value: `0x00` null,
`0x01` array, `0x02` structure, `0x03` boolean, `0x04` bit-string,
`0x09` octet-string, `0x0A` visible-string, `0x17` float32, `0x18` float64.

Octet-strings (`0x09`) matter structurally even though they are not values — the
meter serial and the OBIS codes themselves arrive as octet-strings, and are what
the block algorithm below keys on.

### 5.2 OBIS code structure (IEC 62056-6-1)

`A-B:C.D.E`:

- **A — media**: abstract 0, electricity 1, heat 6, gas 7, water 8
- **B — channel**: 0 when only one exists
- **C — physical quantity**: in 1, out 2; phase current 31/51/71; voltage 32/52/72;
  phase power in 21/41/61, out 22/42/62
- **D — measurement type**: instantaneous 7, counter 8, peak-hold 6
- **E — tariff**: total 0, tariff 1 (day) 1, tariff 2 (night) 2

### 5.3 Telegram structure

The reassembled APDU (§4.2) is a DLMS **DataNotification**, and its shape is
declared by the meter rather than inferred:

```
0F | long-invoke-id-and-priority(4) | 0C <12-byte date-time> | 02 <N> …body…
```

The body is a `structure` of N elements. **Element 0 is an `array` of N
`structure[4]` capture-object definitions** — the push object list — and the
remaining elements are the **values of those objects, positionally, in
declaration order**:

```
02 04 | 12 <class-id:2> | 09 06 <A B C D E FF> | 0F <attribute> | 12 <data-index:2>
```

So definitions come first as a block and values follow as a block; they are not
interleaved, and a single forward pass cannot pair them. Three of the objects
carry non-numeric values — the push-setup object's value *is* the object list
itself, its attribute-1 entry is a logical name, and the clock is a date-time —
so a reader must consume a value for **every** object, not only the ones it
wants, or every value after the first skipped object is attributed to the wrong
register.

**Duplicate registers.** A register can appear more than once in one cycle when
several push lists fire together (§6.1). Taking the first occurrence and ignoring
later ones is the behaviour validated against this meter; it is a choice, and
taking the last would be equally defensible if a later telegram ever proved
fresher.

> **Resolved — the 16-character identifier is a different object.** An earlier
> revision recorded an open question about the meter serial being 8 characters
> here and 16 in published captures, and guessed that `96.1.0` and the COSEM
> logical device name `0-0:42.0.0` might be different objects. Decoding
> `e450_serial16.hex` confirms the guess: `LGZ1030655933512` is the value of
> **`0-0:42.0.0`**, while that same telegram's **`0-0:96.1.1`** is `"1935912"` —
> **seven** characters. So identifier length is a property of the object, and a
> decoder that pattern-matches on an octet-string of any fixed length will find
> nothing at all on a meter configured differently, and will fail silently
> rather than loudly. Treat both which object carries the identity and its length
> as parameters (FR-MTR-05), not constants.

### 5.4 Scaling and precision

**The meter reports in milli-units.** Values arrive as mW, mV, mA and Wh
integers, so every numeric value is multiplied by 0.001 to yield kW, V, A and
kWh. Identifiers arrive as octet strings and are not scaled; their length varies
by object and by configuration (§5.3).

Three decimal places reproduce the meter's integer resolution exactly and add no
false precision, since the underlying value is a milli-unit count.

---

## 6. E450 OBIS Register Catalogue

### 6.1 Meter push lists — as configured by EBL Liestal

Protocols: OBIS (IEC 62056-21 / DLMS COSEM) over M-Bus EN 13757-2.

**This is one DSO's parametrisation, not the meter's capability and not the
vendor's default.** Landis+Gyr publishes a *Standard CIP-Liste für die Schweiz*
(§7.6) of 28 objects — a 15-object Basisliste plus a 13-object Erweiterte Liste —
and EBL's configuration below differs from it in both directions: EBL pushes
four-quadrant reactive energy (`5.8.0`–`8.8.0`) and no voltages, where the
standard list has two-quadrant reactive (`3.8.0`/`4.8.0`), all three voltages and
the active-tariff register `0-0:96.14.0`. Neither is a superset of the other.

A decoder must therefore tolerate registers it has never seen and the absence of
registers it expects — both of which are ordinary, not faults.

Note the four distinct push intervals — a register's update rate is set by which
list it belongs to, so instantaneous power arrives every 5 s while tariff counters
arrive every minute.

| Register | Bezeichnung | Interval |
| -------- | ----------- | -------- |
| `0-0:96.1.0; 2` | Geräteidentifikation 1 (Herstellerseriennummer) | 15 s list |
| `0-0:1.0.0; 2` | Uhr | |
| `0-0:96.13.0; 2` | Konsumenteninformationstext | |
| `0-0:96.13.1; 2` | Konsumenteninformationscode | |
| `0-8:25.9.0; 2` | Objektliste Push-Einstellungen Verbraucherinformation 1 | 5 s list |
| `0-8:25.9.0; 1` | Logischer Name Push-Einstellungen Verbraucherinformation 1 | |
| `1-0:1.7.0; 2` | Wirkleistungsbezug +P | |
| `1-0:2.7.0; 2` | Wirkleistungslieferung −P | |
| `1-1:1.8.0; 2` | Wirkenergiebezug +A (QI + QIV) | |
| `1-1:2.8.0; 2` | Wirkenergielieferung −A (QII + QIII) | |
| `1-1:5.8.0; 2` | Blindenergie +Ri (QI) | |
| `1-1:6.8.0; 2` | Blindenergie +Rc (QII) | |
| `1-1:7.8.0; 2` | Blindenergie −Ri (QIII) | |
| `1-1:8.8.0; 2` | Blindenergie −Rc (QIV) | |
| `1-0:31.7.0; 2` | Strom L1 | |
| `1-0:51.7.0; 2` | Strom L2 | |
| `1-0:71.7.0; 2` | Strom L3 | |
| `0-9:25.9.0; 2` | Objektliste Push-Einstellungen Verbraucherinformation 2 | 1 min list |
| `0-9:25.9.0; 1` | Logischer Name Push-Einstellungen Verbraucherinformation 2 | |
| `1-1:1.8.1; 2` | Wirkenergiebezug +A Tarif 1 | |
| `1-1:1.8.2; 2` | Wirkenergiebezug +A Tarif 2 | |
| `1-1:2.8.1; 2` | Wirkenergielieferung −A Tarif 1 | |
| `1-1:2.8.2; 2` | Wirkenergielieferung −A Tarif 2 | |
| `0-11:25.9.0; 2` | Objektliste Push-Einstellungen Verbraucherinformation 4 | 15 min list |
| `0-11:25.9.0; 1` | Logischer Name Push-Einstellungen Verbraucherinformation 4 | |
| `0-n:24.1.0; 6` | M-Bus Identifikationsnummer Kanal n (n = 1…4) | |
| `0-n:24.2.1; 2` | M-Bus Wert 1 Kanal n (n = 1…4) | |
| `0-n:24.1.0; 9` | M-Bus Gerätetyp Kanal n (n = 1…4) | |
| `0-n:24.2.1; 3` | Einheit/Skalierung M-Bus Wert 1 Kanal n (n = 1…4) | |

The `0-n:24.x` channels carry **sub-meters** (gas, water, heat) that the E450
relays from its own downstream M-Bus. They are pushed whether or not anything
consumes them. Decoding them means accepting `A > 1` and handling `C = 24`, which
an electricity-only decoder does not.

### 6.2 OBIS register → measurement mapping

The short labels are a naming convention, not an interface constraint.

| Label | OBIS `C.D.E` | Quantity | Unit |
| -------- | ------------ | -------- | ---- |
| `SMid` | 96.1.0 (fallback 96.1.1) | Meter serial (8 chars) | — |
| `Pi` | 1.7.0 | Wirkleistungsbezug +P | kW |
| `Po` | 2.7.0 | Wirkleistungslieferung −P | kW |
| `Pi1` `Pi2` `Pi3` | 21.7.0 / 41.7.0 / 61.7.0 | Power in L1/L2/L3 | kW |
| `Po1` `Po2` `Po3` | 22.7.0 / 42.7.0 / 62.7.0 | Power out L1/L2/L3 | kW |
| `U1` `U2` `U3` | 32.7.0 / 52.7.0 / 72.7.0 | Voltage L1/L2/L3 | V |
| `I1` `I2` `I3` | 31.7.0 / 51.7.0 / 71.7.0 | Current L1/L2/L3 | A |
| `Ei` | 1.8.0 | Wirkenergiebezug +A total | kWh |
| `Eo` | 2.8.0 | Wirkenergielieferung −A total | kWh |
| `Ei1` `Ei2` | 1.8.1 / 1.8.2 | +A Tarif 1 / Tarif 2 | kWh |
| `Eo1` `Eo2` | 2.8.1 / 2.8.2 | −A Tarif 1 / Tarif 2 | kWh |
| `Q5` `Q6` `Q7` `Q8` | 5.8.0 / 6.8.0 / 7.8.0 / 8.8.0 | Blindenergie +Ri / +Rc / −Ri / −Rc | kVArh |
| `Q51` `Q52` | 5.8.1 / 5.8.2 | +Ri per tariff | kVArh |
| `Q61` `Q62` | 6.8.1 / 6.8.2 | +Rc per tariff | kVArh |
| `Q71` `Q72` | 7.8.1 / 7.8.2 | −Ri per tariff | kVArh |
| `Q81` `Q82` | 8.8.1 / 8.8.2 | −Rc per tariff | kVArh |

Selection rules that fall out of the register set:

- **D = 7 (instantaneous):** only `E = 0` exists — instantaneous values have no
  tariff split.
- **D = 8 (counters):** `E = 0, 1, 2` exist for `C ∈ {1, 2, 5, 6, 7, 8}` — total
  plus the two tariffs.

Not every register appears in every burst. A reader must report only the values
actually present in the burst it decoded, rather than emitting a fixed schema
with stale or zero placeholders: a zero meaning "not in this burst" is
indistinguishable from a genuine zero reading.

---

## 7. Reference data

The decode rules in §5.3 are **empirical, not standard**. Blocks delimited by the
meter serial, definitions before the ID and values after, a block ending ten
bytes short of the next ID — none of this appears in IEC 62056. It was inferred
by watching this meter. Any implementation written from the prose above will be
approximately right and wrong in some detail that shows up only against real
traffic.

Real telegrams remove that risk: they turn "does the decoder match the
description" into a question answerable on a laptop in milliseconds, with no
meter and no hardware, and they keep answering it every time the decoder changes.
There are two ways to get them, and the first requires no access to the meter.

### 7.1 Published captures

The reference implementation in §8 ships captured E450 telegrams as test data:
two unencrypted sets — one recorded before and one after a 2026 push-list
reconfiguration — plus a deliberately malformed frame for exercising rejection
paths. Its accompanying tests assert active power, active and reactive energy,
voltage L1–L3 and current L1–L3, which covers the register set in §6.2 almost
exactly.

Two things follow. Verifying a decoder no longer requires meter access. And the
two capture sets differing by a push-list reconfiguration is itself evidence that
**the register set is not stable over time** — a DSO can change what the meter
pushes, so a decoder must tolerate registers appearing and disappearing rather
than assuming a fixed layout.

### 7.2 The capture tool is the first firmware

A capture build — configure the UART per §2.2, dump every received byte as hex to
the console — is worth writing before any decoder exists, because it validates
§2 and §3 on their own. If inversion, the pull settings or the baud rate are
wrong, that shows up as garbage in the dump rather than as a decoder that
mysteriously finds no frames. Getting a clean dump proves the physical layer;
everything after it is software that can be developed away from the meter.

### 7.3 What to record

| | |
| --- | --- |
| **Line settings** | 2400 8E1, RX inverted (§2.2) |
| **Minimum useful** | 60 s — covers the 15 s list and several 5 s lists |
| **Preferred** | 20 min — the only way to catch the 15 min list |
| **Form** | hex bytes **with timestamps** |

**Timestamps are not optional.** The cycle boundary in §4.2 is defined by silence,
so a flat byte dump cannot exercise it: the 2000 ms gap rule, and any test of it,
needs the arrival time of each chunk. Record a millisecond timestamp per read, or
per byte if the capture tool makes that cheap — a burst is at most a few hundred
bytes at ~218 bytes/s, so the volume is trivial either way.

Capture the stream **exactly as received**, before any framing or validation.
Frames that fail CRC are part of what a decoder must survive, and discarding them
at capture time removes the evidence needed to test §4.1.

### 7.4 Capturing without the gPlug

Any USB-serial adapter on the meter's data line works, with one obstacle: most
adapters cannot invert RX, and the front-end output is inverted (§2.2). The
options, cheapest first — an ESP32 of any kind, which inverts in the UART
peripheral; a logic analyser, which also gives exact timing for free; or an
inverting buffer ahead of a plain adapter.

### 7.5 Before publishing a recording

The meter serial identifies a specific meter and therefore an address and an
account, and the energy readings themselves are personal data — occupancy is
plainly visible in a power trace. If a recording is committed to a public
repository, substitute the serial consistently throughout, and be deliberate
about how much consumption history goes with it.

### 7.6 Vendor and DSO documentation

| Source | What it settles |
| --- | --- |
| Landis+Gyr, *Endkundenschnittstelle für die Schweiz — Standard CIP-Liste*, 16 Sep 2024 (public) | Wired M-Bus transmission speed **2400 bps**; **HDLC transmit buffer 128 bytes**, which is why every telegram is GBT-fragmented; push transport service (5) HDLC to destination `0-2:22.0.0`; CII client application process ID 103, pre-established; message security policy **Authenticated + Encrypted**, AES-GCM-128; the 28-object standard CIP list |
| EBL Liestal parametrisation | The four push lists and their registers, §6.1 |

#### Push schedule

The vendor document's reference configuration schedules Consumer Information 1
with twelve execution times — *every minute at seconds 00, 05, 10 … 55* — i.e. a
5 s push **aligned to the meter's wall clock**, not a free-running timer. The
other lists are scheduled on the same object at coarser intervals, which is why
several telegrams can coincide (§4.2).

---

## 8. Existing implementations

This interface is not obscure. Four independent implementations decode it, which
matters for two reasons: the protocol details above can be cross-checked against
working code rather than trusted, and a reader need not be written from scratch.

| Project | Language / licence | Fit |
| --- | --- | --- |
| [`esphome-libs/dlms_parser`](https://github.com/esphome-libs/dlms_parser) | C++20, Apache-2.0 | Closest match — see below |
| [`scs/smartmeter-datacollector`](https://github.com/scs/smartmeter-datacollector) | Python, GPL-2.0 | Reference behaviour and the captures in §7.1 |
| [`Gurux/GuruxDLMS.c`](https://github.com/Gurux/GuruxDLMS.c) | ANSI C, GPL-2.0 with commercial option | Complete DLMS, wrong shape |
| [Tasmota](https://github.com/arendst/Tasmota) gPlug script | Berry script, GPL-3.0 | Runs on this exact hardware today |

### 8.1 `dlms_parser`

| | |
| --- | --- |
| Install | `idf.py add-dependency "esphome/dlms_parser^2.1.0"` — published on the Espressif Component Registry |
| Dependencies | none; no ESPHome, Arduino or PlatformIO required |
| Targets | all ESP targets, plus Linux, macOS and Windows for host-side testing |
| Transport | RAW, HDLC and M-Bus, auto-detected from the leading byte, **including multi-frame segmentation** |
| Decoding | OBIS pattern matching, delivering code plus scaled value by callback |
| Encryption | AES-128-GCM with a pluggable crypto backend (mbedTLS is already in ESP-IDF) |
| Meters | names Landis+Gyr E450/E570, and carries a dedicated pattern for Landis+Gyr's swapped OBIS ordering |
| API | `DlmsParser parser(callback)` then `load_default_patterns()`, then `ParseResult parse(std::span<uint8_t>)` — the callback is a constructor argument, and the buffer is modified in place during reassembly |

Multi-frame segmentation is the notable one: it is the burst-assembly problem of
§4.2, already implemented against this meter family. The library building for
Linux as well as ESP targets means the same decoder can be tested on a laptop
against the §7.1 captures and then run unchanged on the device.

Frictions: it is C++20, so a C application needs an `extern "C"` shim or must
compile as C++; and while the component declares all targets, ESP32-C3 is not
named explicitly in its documentation.

### 8.2 The others

**`smartmeter-datacollector`** is the Swiss reference — developed by
Supercomputing Systems and funded by EKZ — and covers E450, E570, E360,
Iskraemeco AM550, Kamstrup OMNIPOWER and Siemens TD-351x over wired M-Bus, HDLC,
DLMS/COSEM and IEC 62056-21. Being Python and GPL-2.0 it is a behavioural
reference and a source of test data rather than something to embed.

**Gurux** is the most complete DLMS implementation available in ANSI C, but it is
built around a *client* that establishes an association and requests objects. The
CII is push-only and the reader never transmits (§1), so most of the library is
unreachable and its framing is entangled with the request/response model.

**Tasmota** decodes this meter on this hardware today, via a Berry script shipped
by the adapter vendor. It is the origin of the milli-unit scaling in §5.4 and of
the short register labels in §6.2. Worth knowing as prior art and as a
cross-check; its firmware model — a script layered on a general-purpose binary,
which a stock OTA update will overwrite — is a different set of trade-offs.

---

## 9. Out of scope

Everything above the decoder is a firmware design decision, not a property of
this interface: output encoding, transport, broker, topic layout, device
identity, configuration, and update mechanism.
