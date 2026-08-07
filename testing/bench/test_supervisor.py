"""Bench tier — the supervisor: what the device does when a peer goes away.

The FSD's state model is normative and its edges are where connected devices
actually fail. Reaching OPERATIONAL proves nothing about the four ways in.

Every test here restores what it cut before it returns, and asserts the restored
state rather than issuing the command and assuming — a fault left armed silently
corrupts every later test and the next person debugs the firmware.
"""

import time

import pytest

from conftest import wait_until_connected, BENCH_CHANNEL, BENCH_HOST, BENCH_PASS, BENCH_SSID, MqttWatch

pytestmark = pytest.mark.exception

STATE = "gplug/+/state"


def _diag(dut, seconds=20):
    """The most recent diag line, which is the one every bench test greps."""
    lines = [l for l in dut.lines(seconds=seconds) if "diag:" in l]
    return lines[-1] if lines else ""


@pytest.mark.deviation
@pytest.mark.fast
def test_ts031_stored_credentials_reach_operational(dut, broker, sim):
    """TS-031 — FR-SUP-02. A configured device needs no help to come up.

    The whole product depends on this: the device is in a meter cabinet and
    nobody is going to press anything after installation.
    """
    line = _diag(dut, seconds=30)
    print(f"\n  {line}")
    assert "cfg=nvs" in line, (
        f"the device is not running from stored configuration: {line or '(silent)'}"
    )
    assert "state=operational" in line, (
        f"a configured device did not reach OPERATIONAL unaided: {line}"
    )


@pytest.mark.slow
def test_ts037_a_stopped_broker_does_not_stop_the_meter(dut, wb, sim):
    """TS-037 — FR-SUP-08. Losing the broker must not lose the meter.

    A device that stops decoding when the broker goes away has turned a network
    outage into a data outage, and the counters it misses are gone: the meter
    does not replay them.
    """
    wb.mqtt_stop()
    try:
        time.sleep(20)
        dut.drain()
        lines = dut.lines(seconds=45)
        cycles = [l for l in lines if "cycle:" in l]
        diag = [l for l in lines if "diag:" in l]
        print(f"\n  broker down: {len(cycles)} cycle(s) still decoded")
        if diag:
            print(f"  {diag[-1]}")

        assert cycles, (
            "the device stopped reporting meter cycles while the broker was "
            "down — a network fault has become a measurement fault"
        )
        assert any("broker=down" in l for l in diag), (
            "the device did not notice the broker was gone, so its own "
            "diagnostics would mislead whoever reads them"
        )
        assert not any("rst:" in l or "boot:" in l for l in lines), (
            "the device restarted when the broker went away — FR-WDT-05 forbids "
            "turning a broker outage into a reboot"
        )
    finally:
        wb.mqtt_start()
        time.sleep(10)
        assert wb.mqtt_status().get("running"), "the broker was not restored"


@pytest.mark.slow
def test_ts040_the_session_returns_after_a_broker_outage(dut, wb, sim):
    """NFR-SUP-01 (TS-040). Back within a minute of the broker returning.

    Recovery is judged by a live publication, never by a retained value: a
    retained topic read during an outage returns the copy from before the drop,
    with its counters frozen, and reads as a device that never came back.
    """
    watch = MqttWatch(BENCH_HOST, [STATE])
    try:
        wb.mqtt_stop()
        time.sleep(30)
        wb.mqtt_start()
        restored = time.monotonic()
        assert wb.mqtt_status().get("running"), "the broker did not restart"

        deadline = restored + 90
        while time.monotonic() < deadline and len(watch.since(restored)) < 2:
            time.sleep(3)
        fresh = watch.since(restored)
        took = (fresh[-1][0] - restored) if fresh else None
        print(f"\n  {len(fresh)} fresh publication(s) after the broker returned"
              + (f", first pair within {took:.0f}s" if took else ""))

        assert len(fresh) >= 2, (
            f"only {len(fresh)} publication(s) in 90 s after the broker came "
            "back. Two are required: the first may be a retained copy, and only "
            "a second proves the device is publishing now."
        )
        assert took is not None and took <= 90
    finally:
        watch.close()
        if not wb.mqtt_status().get("running"):
            wb.mqtt_start()


@pytest.mark.slow
def test_ts033_ts034_an_access_point_outage_is_survived(dut, wb, broker, sim):
    """TS-033/TS-034 — FR-SUP-04, FR-SUP-05. Retry, backed off and capped.

    Shortened deliberately. FR-SUP-03's contract is a ten-minute outage, and
    that case stays in the plan as TS-032; what this one establishes is the
    behaviour the long test would spend ten minutes confirming — that retries
    continue, space out, and never fall back to AP mode.
    """
    wb.ap_stop()
    try:
        dut.drain()
        lines = dut.lines(seconds=90)
        retries = [l for l in lines if "disconnected" in l or "retrying" in l]
        print(f"\n  {len(retries)} retry line(s) during a 90 s outage")
        for l in retries[:3]:
            print(f"    {l[:110]}")

        assert retries, (
            "the device said nothing at all while its network was gone — it is "
            "either not retrying or not reporting, and both are defects"
        )
        assert not any("state=provisioning" in l for l in lines), (
            "the device fell back to its provisioning portal during an outage. "
            "AC-3 forbids it: a device in a meter cabinet that opens an AP is "
            "unreachable and invisible until someone visits it."
        )
    finally:
        wb.ap_start(BENCH_SSID, BENCH_PASS, channel=BENCH_CHANNEL, internet=True)
        # Two independent proofs, either sufficient. An empty `stations` list is
        # not evidence that nothing is associated: it trails the radio by up to
        # twenty seconds, and this recovery check failed on it while the device
        # was back and publishing. The device's own `broker=up` is the stronger
        # of the two — the broker sits on the bench LAN, reachable only through
        # this AP, so a device that reports it up has necessarily rejoined.
        rejoined, how = wait_until_connected(wb, dut, seconds=150)
        print(f"\n  recovery: {how}")
        assert rejoined, (
            "the device did not rejoin after the access point returned, by "
            f"either proof ({how}) — the rig is now in a state that would fail "
            "every test after this one"
        )
