# Host-tier fixtures

Landis+Gyr E450 push telegrams, unencrypted, as captured from Swiss deployments
and published in [`scs/smartmeter-datacollector`](https://github.com/scs/smartmeter-datacollector)
(`tests/testdata/lg_e450.py`).

| File | Bytes | Why it is here |
|---|---|---|
| `e450_serial16.hex` | 752 | Meter serial `LGZ1030655933512` — the 16-character form |
| `e450_serial8.hex` | 338 | Meter serial `44337811` — the 8-character form, **split across a General Block Transfer boundary** (`4433` ends one block, `7811` begins the next) |

Two files rather than one because a single meter model emits both serial lengths
depending on configuration, which is why `serial_len` is a parameter and not a
constant (FR-MTR-05). The second file is the more valuable of the two: anything
that decodes frame by frame instead of reassembling reads a truncated serial from
it, and reads it without error.

Hex is one line per 16 bytes so that a change to a fixture shows as a readable
diff rather than one enormous line.

**These are somebody else's meter readings, not ours.** A capture taken from the
real meter carries a serial that identifies an address and an account, and a
power trace that shows occupancy — substitute the serial consistently before
committing one, per the testing standard.
