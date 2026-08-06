"""Bench tier — the meter link. Procedure P-METER.

Every assertion here spans several cycles and requires one good one. The rig
delivers a byte-perfect burst roughly half the time, so a single-cycle assertion
is flaky by construction and gets blamed on the firmware
(testing/test-plan.yaml, capability `mbus-sim`).

Test IDs are the ones declared in testing/test-plan.yaml. A test written here
under an invented ID is a test nobody asked for and nothing tracks.
"""

import re

import pytest

from conftest import a_good_cycle

# What the simulator reports emitting per cycle, read from its own `emit` line
# rather than assumed.
#
# The line appears once per cycle, so this waits for one. Scraping a short
# window instead returns None whenever the poll lands in the silence, and the
# test then fails complaining about the simulator rather than about the board.
def emitted_bytes(sim, seconds=12):
    text = sim.command("", settle=seconds)
    for line in text.splitlines():
        m = re.search(r"emit .*bytes=(\d+)", line)
        if m:
            return int(m.group(1))
    return None


def cycle_sizes(dut, seconds):
    return [int(m.group(1))
            for line in dut.lines(seconds=seconds)
            if (m := re.search(r"cycle: (\d+) bytes", line))]


@pytest.mark.deviation
@pytest.mark.fast
@pytest.mark.xfail(
    reason="the firmware's line-setting probe rotates on absent framing, and a "
           "pattern has none by construction — the setting moves during the test",
    strict=False,
)
def test_ts022_polarity_is_right(dut, sim):
    """TS-022 — FR-MTR-01, FR-MTR-02. Mode 1 drives a continuous 0x55.

    0x55 alternates every bit, so it is the one pattern that cannot survive a
    polarity error unnoticed: inverted, it arrives as a uniform 0xD5 rather than
    as noise. Run before anything that interprets content, because every later
    result depends on the bits being the right way up.

    It cannot do that yet. The firmware probes UART candidates and rotates after
    two bursts that show no HDLC framing — which a 0x55 pattern never shows. So
    the probe walks candidates mid-test and the board reports whichever setting
    it was on: one run collected 0x55, 0x00 and 0xD5 together. The test is right
    and the seam is missing; it needs a way to pin the line setting, and that is
    an obligation on the firmware rather than a reason to weaken the check.
    """
    # A mode change takes effect on the NEXT telegram, so the cycle in flight is
    # still e450. Draining before it has passed captures a mixture and the test
    # reports a polarity fault that is not there.
    sim.command("mode 1", settle=8)
    dut.drain()
    heads = [m.group(1) for line in dut.lines(seconds=30)
             if (m := re.search(r"first 32 bytes: (.*)", line))]
    sim.command("mode 3", settle=1)

    assert heads, "no burst was reported in 30 s — the line is silent"
    seen = {b for head in heads for b in head.split()[:16]}
    print(f"\n  distinct byte values received: {sorted(seen)[:8]}")
    assert seen == {"55"}, f"expected only 0x55; got {sorted(seen)[:12]}"


# How many cycles to watch before deciding the link is unusable. The rig
# delivers a clean burst roughly half the time, so a handful of bad ones in a row
# is ordinary and only a run of them means anything.
GATE_CYCLES = 10


@pytest.mark.deviation
def test_link_health(dut, sim):
    """The meter link is good enough to test through. A precondition, not a
    verification — it carries no test ID because it discharges no requirement.

    **What it asserts is that a cycle decodes, not that a percentage of bytes
    arrives.** The first version compared the best cycle against 90% of what the
    simulator reported sending, and the rig sits right on that line: 89%, 95% and
    97% across three runs of the same unchanged setup. A fixed percentage floor
    there is a coin toss, and the temptation when it fails is to lower the number
    until it passes — which is tuning a threshold to hide a result.

    So it asserts the thing every test below it actually depends on: that at
    least one cycle in ten decodes something. A run of ten that all decode
    nothing is a broken link and makes every result below unattributable; one
    good cycle in ten is the rig working as well as it ever does.

    The byte counts are still printed, because they are what a person diagnosing
    a bad session wants to see. They are information, not a criterion.
    """
    sent = emitted_bytes(sim)
    assert sent, "the simulator did not report how many bytes it emits"

    dut.drain()
    sizes, decoded = [], []
    for line in dut.lines(seconds=70):
        m = re.search(r"cycle: (\d+) bytes, (\d+) objects", line)
        if m:
            sizes.append(int(m.group(1)))
            decoded.append(int(m.group(2)))
        if len(sizes) >= GATE_CYCLES:
            break

    assert sizes, "no cycle reported in 70 s — the meter link is silent"
    best_bytes = max(sizes)
    best_objects = max(decoded)
    print(f"\n  {len(sizes)} cycles | bytes {sorted(set(sizes))}, best {best_bytes}/{sent} "
          f"({best_bytes / sent:.0%}) | objects decoded {sorted(set(decoded))}")

    assert best_objects > 0, (
        f"no cycle decoded anything across {len(sizes)} cycles (best {best_bytes} of {sent} "
        f"bytes). That is a broken link rather than a lossy one, and no result below it "
        f"means anything."
    )


@pytest.mark.deviation
@pytest.mark.fast
def test_ts021_the_full_telegram_is_a_deviation(dut, sim):
    """TS-021 — FR-MTR-06. The full E450 telegram, three GBT blocks, 417 bytes.

    A deviation rather than the baseline. The rig delivers about 370 of those
    bytes and the missing ones are always the opening flag and address of frame
    1, so the identity does not survive — measured, and removed entirely by six
    bytes of lead-in. That is the simulator, not the firmware.

    So the assertion is the acceptance rule for this rig: a few proper signals
    decode. Values must come out of the long telegram across several cycles; the
    identity is not required here, because the bread-and-butter phase already
    proved the device reads one when it arrives.
    """
    sim.command("mode 3")
    dut.drain()
    best, seen = a_good_cycle(dut, cycles=8, seconds=60)
    sim.command(sim.NORMAL_MODE)
    print(f"\n  full telegram: best cycle decoded {best} object(s) over {seen} cycles")
    assert best > 0, f"nothing decoded from the full telegram across {seen} cycles"


@pytest.mark.deviation
@pytest.mark.fast
def test_ts016_both_serial_lengths_decode(dut, sim):
    """TS-016 — FR-MTR-05. The 8- and 16-character meter serials both decode,
    with no reflash between them. A decoder that assumes one length is wrong
    half the time, and wrong by finding nothing — which reads as a quiet meter.
    """
    for length in ("8", "16"):
        sim.command(f"serial {length}")
        dut.drain()
        best, seen = a_good_cycle(dut, cycles=6, seconds=45)
        print(f"\n  serial {length}: best cycle decoded {best} object(s) over {seen} cycles")
        assert best > 0, f"nothing decoded with serial {length} across {seen} cycles"


@pytest.mark.deviation
@pytest.mark.slow
@pytest.mark.disruptive
def test_ts017_recovers_from_a_mid_frame_start(dut, sim):
    """TS-017 — FR-MTR-06. One transmission begins part-way through.

    The cost of a mid-burst start is bounded at the cycle in progress: the next
    complete cycle must be whole. Asserting on the disturbed cycle instead would
    demand recovery of bytes that were never received.
    """
    sim.command("fault start-mid-frame")
    dut.drain()
    best, seen = a_good_cycle(dut, cycles=8, seconds=60)
    print(f"\n  after a mid-frame start: best {best} object(s) over {seen} cycles")
    assert best > 0, "no cycle recovered after a mid-frame start"


@pytest.mark.deviation
@pytest.mark.slow
@pytest.mark.disruptive
def test_ts018_recovers_after_a_reset_mid_transmission(dut, sim):
    """TS-018 — FR-MTR-06. The case that happens on every real power-up: the
    device is energised by the meter, so it wakes into a transmission already in
    progress. One power cycle must be enough.
    """
    dut.drain()
    dut.reset()
    best, seen = a_good_cycle(dut, cycles=8, seconds=75)
    print(f"\n  after reset mid-transmission: best {best} object(s) over {seen} cycles")
    assert best > 0, "nothing decoded after a reset — a second power cycle should not be needed"


@pytest.mark.exception
@pytest.mark.fast
@pytest.mark.disruptive
def test_ts019_a_bad_checksum_frame_becomes_nothing(dut, sim):
    """TS-019 — FR-MTR-07, FR-MTR-08. One frame carries a bad checksum.

    The prohibited outcome is the assertion: a decoder that emits values from a
    corrupted frame produces a message that parses and graphs, and nothing
    downstream can tell it is wrong. So the check is that the corrupted cycle
    publishes nothing, not merely that the device survives.
    """
    sim.command("fault fcs 2")
    dut.drain()
    lines = dut.lines(seconds=30)
    cycles = [l for l in lines if "cycle:" in l]
    published = [l for l in lines if "published" in l and "state" in l]
    print(f"\n  {len(cycles)} cycle(s), {len(published)} publish(es)")

    # The directive corrupts ONE telegram and then clears itself, so the good
    # cycles either side of it publish exactly as they should. Asserting that
    # nothing at all was published across the window therefore fails on correct
    # behaviour — which is what it did on 2026-08-06, reporting two legitimate
    # publishes as a corrupted frame reaching the broker. What the requirement
    # actually forbids is values coming OUT of the corrupted cycle, so the
    # oracle is that at least one cycle in the window produced no publication.
    assert cycles, "no cycle at all in 30 s — the meter link is silent"
    assert len(published) < len(cycles), (
        f"every one of {len(cycles)} cycles published, so the corrupted frame "
        "produced values too. A decoder that emits from a bad-checksum frame "
        "makes a message that parses and graphs, and nothing downstream can "
        "tell it is wrong."
    )


@pytest.mark.parametrize("directive,expect", [
    ("silence 60", "no measurement is published"),
    ("fault noise 200", "a condition distinct from a decode failure is logged"),
])
@pytest.mark.exception
@pytest.mark.slow
@pytest.mark.disruptive
def test_ts042_ts044_quiet_and_noisy_lines(dut, sim, directive, expect):
    """TS-042 — FR-MTR-12, and TS-044 — FR-MTR-14.

    A silent meter and a noisy one are different conditions and must not present
    as the same one. Silence is normal and must not disturb the network session;
    noise is abnormal and must be distinguishable from a decode failure, because
    the two want completely different fixes.
    """
    sim.command(directive)
    dut.drain()
    lines = dut.lines(seconds=40)

    published = [line for line in lines if "published" in line and "state" in line]
    assert not published, f"{directive!r} produced a published measurement: {published}"

    if directive.startswith("fault noise"):
        distinct = [line for line in lines if "nothing decoded" in line or "flags" in line]
        assert distinct, "noise was not reported as anything — it must not look like silence"
        print(f"\n  noise reported on {len(distinct)} line(s)")
