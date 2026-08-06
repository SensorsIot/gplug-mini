"""Bench tier — what the device puts on the broker, and what it must not.

Everything here is observed at the broker, which FSD §22.0 makes the verification
boundary: the device's own log says what it handed to the MQTT client, and the
gap between that and what arrived is exactly where a wrong topic or a dropped
session hides.

Two habits are load-bearing and were paid for on 2026-08-06:

* **Never assert on a line the device prints once.** The console drops
  characters and goes quiet for stretches, and two tests spent a day reporting a
  working board as broken because a transient had scrolled past. Assert on the
  broker, or on the periodic diag line, or on nothing.
* **Retention cannot be read off a live delivery.** MQTT sets the retain flag
  only on messages the broker replays at subscribe time, so proving a config is
  retained means arriving late and being sent it.
"""

import json
import re
import time

import pytest

from conftest import BENCH_HOST, MqttWatch

pytestmark = pytest.mark.deviation

DISCOVERY = "homeassistant/sensor/+/config"
STATE = "gplug/+/state"
STATUS = "gplug/+/status"


def _labels(topics):
    """`homeassistant/sensor/44337811_active_energy_plus/config` -> the label."""
    return {t.split("/")[-2].split("_", 1)[1] for t in topics}


@pytest.mark.fast
def test_ts045_discovery_is_retained_for_a_late_subscriber(dut, broker, sim, dut_mac):
    """TS-045 — FR-HA-01. One retained config per entity the cycle carried.

    Retained is the whole point: the configs are published once, when the meter
    serial is first read, and a consumer that connects an hour later must still
    learn the entities. So this arrives late on purpose and requires the broker
    to replay them.
    """
    watch = MqttWatch(BENCH_HOST, [DISCOVERY])
    try:
        time.sleep(10)
        replayed = [m for m in watch.messages if m[3]]
    finally:
        watch.close()

    topics = [m[1] for m in replayed]
    print(f"\n  {len(topics)} retained config(s) replayed to a fresh subscriber")
    for t in topics[:4]:
        print(f"    {t}")

    assert topics, (
        "a subscriber connecting now is sent no discovery configs, so the "
        "entities exist only for whoever happened to be listening once"
    )
    assert all(dut_mac.replace(":", "") not in t for t in topics), (
        "a discovery topic is keyed on the device MAC. FR-HA-02 keys them on the "
        "meter serial so history survives replacing the hardware"
    )


@pytest.mark.fast
def test_ts050_state_messages_are_not_retained(dut, broker, sim):
    """TS-050 — FR-HA-07. A measurement must not outlive its moment.

    A retained reading is indistinguishable from a live one of the same value,
    and in an energy dashboard that difference is the entire point. This is the
    same concern as FR-HA-05 seen from the other side.
    """
    watch = MqttWatch(BENCH_HOST, [STATE])
    try:
        time.sleep(8)
        replayed = [m for m in watch.messages if m[3]]
        live = [m for m in watch.messages if not m[3]]
    finally:
        watch.close()

    print(f"\n  {len(replayed)} retained, {len(live)} live state message(s)")
    assert not replayed, (
        f"the broker replayed {len(replayed)} state message(s) to a fresh "
        "subscriber, so a stale reading is served as though it were current"
    )


@pytest.mark.fast
def test_ts087_operational_topics_are_keyed_on_the_mac(dut, broker, sim, dut_mac):
    """TS-087 — FR-ID-02. Two devices on one broker must never collide.

    Entities key on the meter serial so history survives a hardware swap
    (FR-ID-01); the operational topics key on the MAC for the opposite reason —
    two gPlugs reading two meters must not publish over each other.
    """
    watch = MqttWatch(BENCH_HOST, [STATE, STATUS])
    try:
        time.sleep(12)
        topics = {m[1] for m in watch.messages}
    finally:
        watch.close()

    print(f"\n  operational topics: {sorted(topics)}")
    assert topics, "no operational topic reached the broker in 12 s"
    for t in topics:
        assert t.split("/")[1] == dut_mac, (
            f"{t} is not keyed on this device's MAC ({dut_mac}) — a second "
            "device would publish over it"
        )


@pytest.mark.slow
def test_ts052_a_measurement_is_published_within_the_budget(dut, broker, sim):
    """NFR-HA-01 (TS-052). Two seconds from cycle boundary to the broker.

    Measured as the gap between the device announcing a completed cycle and the
    state message arriving, which is the interval the requirement is about. The
    device's own clock and ours are not synchronised, so this compares two
    events observed here rather than two timestamps from different sources.
    """
    watch = MqttWatch(BENCH_HOST, [STATE])
    try:
        dut.drain()
        deadline = time.monotonic() + 60
        gaps = []
        while time.monotonic() < deadline and len(gaps) < 3:
            for line in dut.lines(seconds=6):
                if "published" in line and "state" in line:
                    seen_at = time.monotonic()
                    arrivals = [m for m in watch.messages if m[0] >= seen_at - 6]
                    if arrivals:
                        gaps.append(abs(arrivals[-1][0] - seen_at))
    finally:
        watch.close()

    print(f"\n  {len(gaps)} publication(s) observed, gaps {[round(g, 2) for g in gaps]}")
    assert gaps, (
        "no publication was observed at both the device and the broker in 60 s, "
        "so the latency could not be measured at all"
    )
    # The serial poll granularity is seconds, so this cannot resolve a 2 s budget
    # precisely — it can only catch a gross violation. Recorded as such rather
    # than pretending to a precision the rig does not have.
    assert max(gaps) < 10, (
        f"a measurement took {max(gaps):.1f}s to reach the broker after the "
        "device said it published — far outside the 2 s budget"
    )


@pytest.mark.exception
@pytest.mark.slow
def test_ts047_no_discovery_without_a_meter_serial(dut, broker, sim, wb):
    """TS-047 — FR-HA-03. Discovery waits for the meter identity.

    Published early, the entities key on a placeholder; the real serial then
    creates a second set and splits a household's history in two. Observed as an
    ordering — the broker is cleared and the meter is made anonymous BEFORE the
    device reconnects — because looking only at the end cannot tell "published
    after" from "published all along".
    """
    watch = MqttWatch(BENCH_HOST, [DISCOVERY])
    try:
        time.sleep(6)
        watch.clear_retained([m[1] for m in watch.messages])
        sim.set_verified("identity", "none")

        dut.reset()          # a fresh session, so discovery would be republished
        mark = watch.mark()
        time.sleep(45)
        premature = watch.since(mark, "homeassistant/")
        print(f"\n  45 s with an anonymous meter: {len(premature)} config(s)")
        assert not premature, (
            f"discovery was published with no meter serial known: "
            f"{[m[1] for m in premature][:3]}"
        )

        sim.set_verified("identity", "ldn")
        mark = watch.mark()
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline and not watch.since(mark, "homeassistant/"):
            time.sleep(3)
        after = watch.since(mark, "homeassistant/")
        print(f"  {len(after)} config(s) once the identity arrived")
        assert after, (
            "the meter identified itself and no discovery followed — the device "
            "defers correctly but never resumes, so a consumer sees nothing"
        )
    finally:
        watch.close()


@pytest.mark.exception
@pytest.mark.slow
def test_ts046_discovery_is_restated_on_a_new_session(dut, wb, sim, dut_mac):
    """FR-HA-01 (TS-046). A broker that forgot must be told again.

    Retained discovery is a statement to the BROKER, not a fact about the
    device, and a broker that restarts without persistence has forgotten it —
    which this bench's mosquitto does every single time, having no persistence
    configured. Until 2026-08-06 the firmware latched `discovery_done` once per
    boot, so after any broker restart the entities were gone until somebody
    power-cycled a device that lives in a meter cabinet.

    The device is deliberately NOT reset here. A reboot would republish under
    either behaviour and prove nothing; the whole question is whether a new
    session alone is enough.
    """
    watch = MqttWatch(BENCH_HOST, [DISCOVERY])
    try:
        time.sleep(8)
        before = {m[1] for m in watch.messages}
        assert before, "no discovery configs to begin with — nothing to lose"
        print(f"\n  {len(before)} config(s) present before the outage")

        # Forget them, then break the session without touching the device.
        watch.clear_retained(sorted(before))
        wb.mqtt_stop()
        time.sleep(15)
        wb.mqtt_start()
        assert wb.mqtt_status().get("running"), "the broker did not come back"
        restarted = watch.mark()

        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            if len({m[1] for m in watch.since(restarted, "homeassistant/")}) >= len(before):
                break
            time.sleep(5)
        after = {m[1] for m in watch.since(restarted, "homeassistant/")}
        print(f"  {len(after)} config(s) restated on the new session")

        assert after, (
            "the device reconnected and never restated its discovery configs. "
            "The broker has forgotten the entities and the device believes it "
            "has already said them, so a consumer sees nothing until someone "
            "physically power-cycles a device in a meter cabinet."
        )
        assert after >= before, (
            f"only {len(after)} of {len(before)} configs came back: "
            f"{sorted(before - after)}"
        )
    finally:
        watch.close()
        if not wb.mqtt_status().get("running"):
            wb.mqtt_start()
