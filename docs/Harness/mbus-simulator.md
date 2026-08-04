# Driving gPlug-mini from an M-Bus simulator

Instructions for whoever operates the simulator. The device currently receives
~319 bytes per burst and decodes nothing from them, and the reason is not yet
known — these stages separate the two candidate causes instead of testing both
at once.

## What we see now

```
cycle: 319 bytes, 0 objects, 2 consumed
first 32 bytes: B7 2F 06 00 00 00 00 00 00 00 00 00 78 7F 9F 9F E1 01 FF FF FB 79 ...
0x7E flags: 1, first at offset 158 of 319
```

Bytes are arriving, so the wiring and the direction are right. But a valid
telegram is a sequence of HDLC frames, each opening and closing with `7E`, and
319 bytes contain **one** `7E` — which in that much noise is chance, not framing.
The `7F / 9F / FF / EF` pattern is what a UART produces when the bit polarity,
parity or rate does not match: it yields bytes rather than silence, which is the
hard case, because it looks like data.

So either the line settings differ from what we expect, or the content is not
what we expect. One test at a time will say which.

## What the device expects on the wire

| | |
|---|---|
| Rate | 2400 baud |
| Frame | 8 data bits, **even** parity, 1 stop bit |
| Polarity | **Inverted** at the meter's customer interface (interface spec §2.2) |
| Direction | Receive only — the device never transmits |
| Idle | Silent between transmissions |

The firmware probes the four plausible combinations (8E1/8N1 × inverted/normal)
if it sees no framing, so it should settle on the right one by itself. Telling us
which one the simulator drives is still faster than letting it search.

## Stage 1 — the line, with no protocol at all

**Send `0x55` continuously, at 2400 baud, for ten seconds or so.**

`0x55` is `01010101`, so every bit alternates. It is the standard pattern for
this: if the rate and polarity are right, every received byte is `0x55`; if the
polarity is inverted relative to what we expect, they come out `0xAA`; if the
rate is wrong, they come out as neither, consistently.

Nothing else needs to be right for this — no framing, no checksums, no content.

Tell us what the device logs and we will know the line settings exactly. That
answers the question this whole exercise is stuck on.

## Stage 2 — one real telegram, replayed verbatim

Once Stage 1 is clean, send **exactly the bytes below**, then go silent for at
least 3 seconds.

These are a published capture from a real Landis+Gyr E450, and our host tests
already decode them: meter serial `44337811`, active power 777 W, active energy
25 149 419 Wh. Because the expected result is known exactly, anything that comes
out wrong is a fault on the wire rather than a question about content.

Send them as one continuous stream — the three HDLC frames back to back, with no
added gaps between them:

- frame 1: 134 bytes, starts `7E A0 84 CE FF 03`
- frame 2: 141 bytes, starts `7E A0 8B CE FF 03`
- frame 3: 63 bytes, starts `7E A0 3D CE FF 03`

The full byte sequence is in this repository at
`test/host/fixtures/e450_serial8.hex`, one line per 16 bytes, and is reproduced
at the end of this document.

**Do not add or strip anything.** In particular the value that identifies the
meter is split across the second and third frames — `4433` ends one and `7811`
begins the next — so a simulator that "helpfully" pads or realigns frames will
break exactly the case we most want to test.

## Stage 3 — cycles

Repeat Stage 2 on a cycle, with **at least 3 seconds of silence** between
transmissions.

The silence is not cosmetic: the device treats a gap of 2000 ms as the end of a
transmission and only then decodes. A simulator that streams continuously, or
that pauses for less than 2 seconds, will have its frames merged and decoded as
one malformed telegram. Ten seconds between cycles matches what the real meter
does and is a good default.

Scrambled bytes at the very start of a transmission are fine — the device is
required to recover alignment when reception begins mid-burst — as long as the
frames that follow are intact.

## What good looks like

At Stage 2 or 3 the device logs, per cycle:

```
gplug: meter_serial         = 44337811
gplug: active_power_plus    = 777.000 W
gplug: active_energy_plus   = 25149419 Wh (scaler 0)
gplug: cycle: 338 bytes, 10 objects, 260 consumed
```

`10 objects` is the number to watch. `0 objects` means nothing decoded, and the
device prints the head of the buffer and whether it found any framing.

## The bytes for Stage 2

```
7E A0 84 CE FF 03 13 12 8B E6 E7 00 E0 40 00 01
00 00 70 0F 00 03 B5 33 0C 07 E6 0B 16 02 10 25
1E FF 80 00 00 02 0B 01 0B 02 04 12 00 28 09 06
00 08 19 09 00 FF 0F 02 12 00 00 02 04 12 00 28
09 06 00 08 19 09 00 FF 0F 01 12 00 00 02 04 12
00 01 09 06 00 00 60 01 00 FF 0F 02 12 00 00 02
04 12 00 03 09 06 01 00 01 07 00 FF 0F 02 12 00
00 02 04 12 00 03 09 06 01 00 02 07 00 FF 0F 02
12 00 00 4C 21 7E 7E A0 8B CE FF 03 13 EE E1 E0
40 00 02 00 00 7A 02 04 12 00 03 09 06 01 01 01
08 00 FF 0F 02 12 00 00 02 04 12 00 03 09 06 01
01 02 08 00 FF 0F 02 12 00 00 02 04 12 00 03 09
06 01 01 05 08 00 FF 0F 02 12 00 00 02 04 12 00
03 09 06 01 01 06 08 00 FF 0F 02 12 00 00 02 04
12 00 03 09 06 01 01 07 08 00 FF 0F 02 12 00 00
02 04 12 00 03 09 06 01 01 08 08 00 FF 0F 02 12
00 00 09 06 00 08 19 09 00 FF 09 08 34 34 33 33
8B 52 7E 7E A0 3D CE FF 03 13 F2 84 E0 C0 00 03
00 00 2C 37 38 31 31 06 00 00 03 09 06 00 00 00
00 06 01 7F BF EB 06 00 43 7A DE 06 00 2F CD AE
06 00 00 33 BF 06 00 37 70 2E 06 00 F0 41 58 40
EF 7E
```
