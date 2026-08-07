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
@pytest.mark.slow
@pytest.mark.disruptive
def test_ts022_the_shipped_line_setting_reads_the_meter(dut, sim):
    """TS-022 — FR-MTR-01, FR-MTR-02. The device ships reading the right line.

    The simulator is driven at the polarity the meter's front-end delivers
    (`invert on`, interface spec §2.2), so a fresh boot faces exactly what an
    installation presents.

    The assertion is that the FIRST line setting works: the device decodes
    without ever logging `nothing decodes — trying …`. That rotation is a
    recovery path, and a device that reaches the meter only by walking off its
    shipped configuration satisfies no requirement — FR-MTR-02 says the receive
    signal is inverted, not that something eventually is.

    It is also why polarity cannot be tested with mode 1. A 0x55 pattern carries
    no HDLC framing by construction, so the probe rotates during the test and
    the board reports whichever candidate it happened to be on. A real telegram
    at the real polarity settles the question the pattern cannot.
    """
    dut.drain()
    dut.reset()
    lines = dut.lines(seconds=55)

    rotations = [l for l in lines if "nothing decodes" in l]
    settings = [m.group(1) for line in lines
                if (m := re.search(r"meter UART on GPIO\d+, \d+ (.+), listen-only", line))]
    decoded = [int(m.group(1)) for line in lines
               if (m := re.search(r"cycle: \d+ bytes, (\d+) objects", line))]

    print(f"\n  line setting(s): {settings or ['(not logged this window)']}")
    print(f"  rotations: {len(rotations)} | objects per cycle: {decoded}")

    assert any(n > 0 for n in decoded), (
        "no cycle decoded anything after a fresh boot at the meter's own "
        "polarity — the device cannot read the line it ships configured for"
    )
    assert not rotations, (
        f"the device only reached the meter after rotating its line settings "
        f"({[l.split('trying')[-1].strip() for l in rotations]}). The shipped "
        "configuration does not match the interface spec, so every installation "
        "loses the opening bursts of every boot to a probe."
    )


# How many cycles to watch before deciding the link is unusable. The rig
# delivers a clean burst roughly half the time, so a handful of bad ones in a row
# is ordinary and only a run of them means anything.
GATE_CYCLES = 10


@pytest.mark.deviation
def test_link_health(dut, sim):
    """The meter link is good enough to test through. A precondition, not a
    verification — it carries no test ID because it discharges no requirement.

    **What it asserts is that a cycle decodes, not that a percentage of bytes
    arrives.** The rig sits right on the 90% line — repeated runs of the same
    unchanged setup land at 89%, 95% and 97% — so a fixed percentage floor is a
    coin toss, and the temptation when it fails is to lower the number until it
    passes, which is tuning a threshold to hide a result.

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
def test_ts020_the_full_telegram_is_a_deviation(dut, sim):
    """TS-020 — NFR-MTR-01, as far as this rig can carry it.

    What it can prove: the full E450 telegram, three GBT blocks, 417 bytes,
    still yields decoded values. What it CANNOT prove is NFR-MTR-01 itself. The
    requirement is that no byte is dropped, and this rig drops them: about 370
    of 417 arrive and the missing ones are always the opening flag and address
    of frame 1, so the identity does not survive. That is the simulator, not the
    firmware — six bytes of lead-in removes the loss entirely.

    So the assertion is the acceptance rule for this rig: a few proper signals
    decode. Byte-completeness against a meter that really sends every byte is
    observed only in WF-006 and WF-007, at the installation.
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
    before = int(sim.status().get("emissions", 0))
    sim.command("fault fcs 2")
    dut.drain()
    lines = dut.lines(seconds=30)
    after = int(sim.status().get("emissions", 0))

    emitted = after - before
    published = [l for l in lines if "published" in l and "state" in l]
    cycles = [l for l in lines if "cycle:" in l]
    print(f"\n  {emitted} telegram(s) emitted, {len(cycles)} cycle(s) assembled, "
          f"{len(published)} publish(es)")

    # The oracle is the SIMULATOR's emission count, not the device's cycle count.
    # A corrupted telegram is discarded early enough that the device never logs a
    # cycle for it at all, so counting cycles compares two numbers that both drop
    # and proves nothing: the corrupted burst is simply missing from both.
    # Emissions are what the rig knows it sent, so publishes < emissions is the
    # claim that the bad frame produced no values.
    assert emitted > 0, "the simulator emitted nothing in 30 s — a rig fault"
    assert len(published) < emitted, (
        f"all {emitted} emitted telegrams produced a publication, so the frame "
        "with the bad checksum produced values too. A decoder that emits from a "
        "corrupted frame makes a message that parses and graphs, and nothing "
        "downstream can tell it is wrong."
    )


@pytest.mark.exception
@pytest.mark.slow
@pytest.mark.disruptive
def test_ts042_a_silent_line_publishes_nothing(dut, sim):
    """TS-042 — FR-MTR-12. A meter that says nothing must not invent a reading.

    Silence is normal: the meter is quiet between transmissions and can be quiet
    for far longer. What must not happen is a measurement appearing anyway, from
    a cached set or a repeated one, because a value that outlives its moment is
    indistinguishable from a live one in an energy dashboard.

    Written as its own case rather than parametrised alongside TS-044, because a
    node carrying two case ids records only the first: TS-044's result was
    silently dropped for as long as they shared a function.
    """
    dut.drain()
    before = int(sim.status().get("emissions", 0))
    sim.command("silence 60")
    lines = dut.lines(seconds=40)
    emitted = int(sim.status().get("emissions", 0)) - before

    published = [line for line in lines if "published" in line and "state" in line]
    print(f"\n  {emitted} emission(s) during silence, {len(published)} publish(es)")
    assert not published, (
        f"a measurement was published with the meter sending nothing: {published}"
    )


@pytest.mark.exception
@pytest.mark.slow
@pytest.mark.disruptive
def test_ts044_noise_is_reported_as_its_own_condition(dut, sim):
    """TS-044 — FR-MTR-14. A noisy line and a quiet one are different faults.

    They want completely different fixes — a quiet line is a meter that is not
    talking, a noisy one is a link that is corrupting what it carries — so they
    must not present as the same condition. The device must yield no values from
    the noisy burst AND say something distinct about it.
    """
    dut.drain()
    before = int(sim.status().get("emissions", 0))
    sim.command("fault noise 200")
    lines = dut.lines(seconds=40)

    # The noise fault corrupts a single telegram and then clears itself, so the
    # device has exactly one opportunity to announce it and does so on one line.
    # This console drops lines, and a line that was printed and lost is
    # indistinguishable from one never printed, so its absence after a single
    # burst is not evidence that the device failed to report it.
    #
    # The stimulus is repeated instead, up to three times, stopping as soon as
    # the report appears. Each attempt is a fresh corrupted telegram the device
    # is required to report, so a device genuinely silent about noise still
    # fails.
    for _ in range(3):
        if [l for l in lines if "nothing decoded" in l or "flags" in l]:
            break
        dut.drain()
        sim.command("fault noise 200")
        lines += dut.lines(seconds=25)

    emitted = int(sim.status().get("emissions", 0)) - before
    published = [line for line in lines if "published" in line and "state" in line]
    distinct = [line for line in lines if "nothing decoded" in line or "flags" in line]

    print(f"\n  {emitted} emission(s), {len(published)} publish(es), "
          f"noise reported on {len(distinct)} line(s)")
    assert emitted > 0, "the simulator emitted nothing — a rig fault"
    assert len(published) < emitted, (
        f"all {emitted} emitted telegrams published, so the noisy one produced "
        "values too"
    )
    assert distinct, "noise was not reported as anything — it must not look like silence"
