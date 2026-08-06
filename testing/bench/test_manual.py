"""Bench tier — the cases that need a person, and exactly what that person does.

These are not second-class tests. Each one covers a contract no other case
reaches, and each blocks on `POST /api/human-interaction`, which puts the
instruction on the workbench's web UI and waits for Done or Cancel.

    pytest testing/bench/test_manual.py --wt-url http://<bench>:8080

Run them when you can watch the bench. Deselect them otherwise:

    pytest testing/bench -m "not manual"

What the operator is asked to do, in full, so it can be read before starting:

  TS-035  hold the device button for 5 seconds while it is running
  TS-056  hold the device button down through a power cycle, then release
  TS-049  unplug the device's USB lead, wait 10 s, plug it back in

Nothing here can be automated on this bench: SLOT1 is a native-USB part, the
Pi's GPIO header reaches nothing on the board, and the slot has no switched
supply. That is recorded as the `button-gpio` and `power-cycle-endurance`
capabilities being unavailable — and the reason `physical-button` and
`power-cycle` are available is precisely that a person can do them.
"""

import time

import pytest

from conftest import BENCH_HOST, MqttWatch

pytestmark = [pytest.mark.manual, pytest.mark.disruptive, pytest.mark.slow]

STATUS = "gplug/+/status"
PROMPT_TIMEOUT = 180


def _ask(wb, message):
    """Put an instruction on the bench UI and wait. Skip, never fail, on refusal.

    A test the operator did not perform has established nothing, and recording
    that as a failure would claim knowledge about the requirement that nobody
    has — the same rule as a failed precondition being `not done`.
    """
    print(f"\n  >>> {message}")
    confirmed = wb.human_interaction(message, timeout=PROMPT_TIMEOUT)
    if not confirmed:
        pytest.skip(
            "the operator did not confirm the action, so nothing was learned "
            "about this requirement"
        )


@pytest.mark.exception
def test_ts035_a_five_second_hold_reopens_the_portal(dut, wb, broker, sim):
    """TS-035 — FR-SUP-06. The only way back into a device already installed.

    Without this a device whose network changed is scrap: it is in a meter
    cabinet, it holds credentials that no longer work, and nothing short of
    physical removal reaches it. The button is the whole recovery story.

    OPERATOR: press and hold the button on the gPlug for a full five seconds,
    then release. The device should raise its `gplug-xxxxxx` setup network.
    """
    before = _state_line(dut)
    print(f"  before: {before}")
    assert "state=operational" in before, (
        "the device is not OPERATIONAL to begin with, so a hold would not be "
        "testing what this case is about"
    )

    _ask(wb, "Hold the button on the gPlug for 5 full seconds, then release it. "
             "Click Done when you have released it.")

    line = dut.await_line(r"state=provisioning ssid=(gplug-[0-9a-f]{6})", seconds=60)
    print(f"  after:  {line}")
    assert "provisioning" in line, (
        "a five-second hold did not reopen the portal, so an installed device "
        "whose network changed cannot be recovered without removing it"
    )


@pytest.mark.deviation
def test_ts056_the_button_does_not_strap_the_board_into_download_mode(dut, wb):
    """TS-056 — FR-PRV-04. Held through a reset, the board must still boot.

    The device button is on GPIO9, which the ROM samples at reset to decide
    between normal boot and serial download. A user holding the button while
    power returns — which is exactly what someone does when they think they are
    resetting it — must not leave a device that looks dead.

    OPERATOR: hold the button down, and WHILE HOLDING IT unplug the USB lead,
    wait a few seconds, plug it back in, keep holding for another 3 seconds,
    then release.
    """
    _ask(wb, "Hold the gPlug's button DOWN. While holding it: unplug the USB "
             "lead, wait 3 s, plug it back in, keep holding 3 s more, then "
             "release. Click Done when released.")

    # Proven off the console, because the console cannot prove it. After a
    # physical replug the CDC re-enumerates and stays silent for long stretches —
    # measured 2026-08-06, 90 s of nothing from a board that had booted fine —
    # so "no banner" is not evidence of a failure to boot.
    #
    # A board in SPI download boot runs NO firmware: it cannot join a network and
    # cannot publish. So a station on this AP, or a message on the broker, is
    # positive proof that the ROM chose normal boot with the button held.
    time.sleep(20)
    watch = MqttWatch(BENCH_HOST, [STATUS])
    try:
        deadline = time.monotonic() + 150
        booted = None
        while time.monotonic() < deadline:
            if watch.messages:
                booted = f"published {watch.messages[-1][1]}"
                break
            if (wb.ap_status() or {}).get("stations"):
                booted = f"joined as {(wb.ap_status() or {})['stations'][0]['mac']}"
                break
            time.sleep(5)
    finally:
        watch.close()

    print(f"\n  {booted or 'no sign of the firmware running'}")
    assert booted, (
        "the board neither joined the network nor published anything in 150 s "
        "after the button was held through a power cycle. That is what SPI "
        "download boot looks like from outside: no firmware runs, so a user who "
        "holds the button while power returns is left with a device that answers "
        "nothing and looks dead."
    )


@pytest.mark.exception
def test_ts049_an_unclean_power_loss_fires_the_last_will(dut, wb, broker, sim):
    """TS-049 — FR-HA-06. The broker says the device is gone, without being told.

    A device that dies mid-cycle cannot announce it. The last will is the
    broker's job, and the only way to prove it fires is to kill the device
    without a clean disconnect — which on this bench means pulling the lead.

    A sentinel is published first: the will topic is retained, so a stale
    `offline` from a previous run is indistinguishable from a fresh one, and
    reading it without seeding would prove nothing at all.

    OPERATOR: unplug the gPlug's USB lead, wait about 10 seconds, plug it back
    in.
    """
    import paho.mqtt.publish as publish

    watch = MqttWatch(BENCH_HOST, [STATUS])
    try:
        time.sleep(6)
        topics = {m[1] for m in watch.messages}
        assert topics, "the device is publishing no status topic to seed"
        status_topic = sorted(topics)[0]

        publish.single(status_topic, "sentinel", qos=1, retain=True,
                       hostname=BENCH_HOST, port=1883)
        time.sleep(2)
        mark = watch.mark()

        _ask(wb, "Unplug the gPlug's USB lead, wait 10 seconds, then plug it "
                 "back in. Click Done once it is plugged back in.")

        deadline = time.monotonic() + 120     # the will waits out the keepalive
        while time.monotonic() < deadline:
            wills = [m for m in watch.since(mark, status_topic) if m[2] == "offline"]
            if wills:
                print(f"\n  last will fired: {wills[0][1]} -> {wills[0][2]}")
                break
            time.sleep(5)
        else:
            pytest.fail(
                "no `offline` arrived on the status topic within 120 s of the "
                "device losing power. The sentinel proves this is not a stale "
                "retained value: the will did not fire, so a dead device is "
                "shown as available indefinitely."
            )
    finally:
        watch.close()


def _state_line(dut, seconds=25):
    lines = [l for l in dut.lines(seconds=seconds) if "diag:" in l]
    return lines[-1] if lines else ""
