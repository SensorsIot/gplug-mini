"""Bench tier, phase 2 — the device does its job.

Nothing else in the suite is worth reading until this passes. A device that is
not delivering measurements cannot tell you anything useful about how it handles
a corrupted frame, a silent line or a rolled-back update: every one of those
tests fails for the same upstream reason and the report reads as a dozen defects.

There was no test here until 2026-08-05, and its absence cost a day. The suite
had cases for both serial lengths, mid-burst starts, resets, corrupted
checksums, silence and noise — deviations and malfunctions, every one — and
nothing that said "a measurement reached the broker". So a defect that stopped
the device publishing entirely was found sideways, when unrelated OTA tests
errored for want of a topic name.

The chain, in the order it has to work:

    telegram decoded  ->  meter identity read  ->  discovery published
                      ->  state message published

Each step is asserted separately, because they fail for different reasons and
the difference is what tells you where to look.
"""

import re

import pytest

from conftest import BENCH_HOST

pytestmark = pytest.mark.breadandbutter


@pytest.mark.fast
def test_ts108_the_normal_signal_decodes(dut, sim):
    """TS-108 — FR-MTR-09. The first link: bytes on the wire become labelled values.

    Step 4 of the journey, and it needs its own ID rather than riding on a
    neighbouring test — FR-MTR-09 was proven only at host tier against a fixture,
    so until this runs the register mapping has never been shown to hold on
    hardware.

    Asserted over several cycles and requiring one good one — the rig delivers a
    clean burst roughly half the time, so a single-cycle assertion here is flaky
    by construction and gets blamed on the firmware.
    """
    dut.drain()
    counts = [int(m.group(1))
              for line in dut.lines(seconds=45)
              if (m := re.search(r"cycle: \d+ bytes, (\d+) objects", line))]

    assert counts, "no cycle reported in 45 s — the meter link is silent"
    print(f"\n  {len(counts)} cycles, objects decoded {sorted(set(counts))}")
    assert max(counts) > 0, (
        f"no cycle decoded anything across {len(counts)} cycles — "
        "the device is receiving bytes and making nothing of them"
    )


@pytest.mark.fast
def test_ts109_the_meter_identity_is_read(dut, sim):
    """TS-109 — FR-MTR-10. The second link, and the one that gates everything.

    Home Assistant discovery waits on the meter serial (FR-HA-03), so a device
    that never reads it connects, decodes, and publishes nothing — while every
    log line looks healthy. That is precisely the failure this suite missed for
    a day, and it is why the identity gets its own assertion rather than being
    folded into "does it publish".
    """
    state = sim.known_state()
    assert state["identity"] != "none", "the simulator is emitting no identity to read"

    dut.drain()
    lines = dut.lines(seconds=60)
    identity = [line for line in lines if "meter_serial" in line]
    deferred = [line for line in lines if "no meter serial yet" in line]

    print(f"\n  identity lines: {len(identity)}, 'no serial yet' lines: {len(deferred)}")
    if identity:
        print(f"  {identity[0]}")
    assert identity, (
        f"the meter serial was never read in 60 s ({len(deferred)} cycles deferred "
        "discovery for want of it). Nothing will ever be published."
    )


@pytest.mark.slow
def test_ts110_a_measurement_reaches_the_broker(dut, broker, sim):
    """TS-110 — FR-AGG-03, FR-AGG-06. Step 5: the whole chain, observed at the end.

    The device's own log is not enough here. It reports what it handed to the
    MQTT client, not what the broker received, and the gap between those two is
    exactly where a wrong topic or a dropped session hides. So this subscribes
    and waits for the message.

    Two arrivals are required, not one. The first may be a retained copy from
    before this test began — a corpse with its counters frozen — and reading it
    as proof of life is the standard way to conclude a dead device is healthy.
    """
    import paho.mqtt.client as mqtt

    received = []

    def on_message(_client, _userdata, msg):
        received.append((msg.topic, msg.payload.decode("utf-8", "replace")))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = on_message
    client.connect(BENCH_HOST, 1883, 60)
    client.subscribe("gplug/+/state", qos=0)
    client.loop_start()
    try:
        deadline = 90
        import time as _t
        end = _t.monotonic() + deadline
        while _t.monotonic() < end and len(received) < 2:
            _t.sleep(1)
    finally:
        client.loop_stop()
        client.disconnect()

    print(f"\n  {len(received)} state message(s) in 90 s")
    for topic, payload in received[:2]:
        print(f"  {topic}  {payload[:120]}")

    assert received, (
        "no state message reached the broker in 90 s. The device may be decoding "
        "perfectly and publishing nothing — check the identity test above."
    )
    assert len(received) >= 2, (
        "only one message arrived, which may be a retained copy from before this "
        "test. A live publish is what proves the device is working now."
    )

    _topic, payload = received[-1]
    assert payload.startswith("{") and payload.endswith("}"), \
        f"the payload is not one complete JSON object: {payload[:80]}"
    assert len(payload) > 2, "an empty set was published — FR-AGG-06 forbids it"


@pytest.mark.slow
def test_ts111_discovery_is_published(dut, broker, sim):
    """TS-111 — FR-HA-02, FR-HA-04. Entities exist, keyed on the meter.

    Retained, so a subscriber arriving later still learns the entities. That is
    what makes this observable at all — the configs were published once, when the
    serial was first read, possibly long before this test ran.
    """
    import paho.mqtt.client as mqtt

    configs = []

    def on_message(_client, _userdata, msg):
        configs.append((msg.topic, msg.payload.decode("utf-8", "replace")))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = on_message
    client.connect(BENCH_HOST, 1883, 60)
    client.subscribe("homeassistant/sensor/+/config", qos=0)
    client.loop_start()
    try:
        import time as _t
        _t.sleep(20)   # retained messages arrive at once; this is generous
    finally:
        client.loop_stop()
        client.disconnect()

    print(f"\n  {len(configs)} retained discovery config(s)")
    for topic, _ in configs[:3]:
        print(f"  {topic}")

    assert configs, (
        "no discovery config is retained on the broker — Home Assistant would "
        "show no entities at all"
    )
    # FR-HA-04: unique_id keys on the meter serial so history survives replacing
    # the hardware. A MAC-keyed id splits a household's history on every swap.
    assert any("unique_id" in payload for _, payload in configs), \
        "a discovery config carries no unique_id"
