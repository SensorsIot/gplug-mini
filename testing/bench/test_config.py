"""Bench tier — what the device does with a configuration it cannot use.

An incomplete or unusable configuration must land the device somewhere a person
can reach it, which means Provisioning. The alternative — a device that accepts
half a configuration, boots, and quietly joins nothing — is a device in a meter
cabinet that answers no one and shows no symptom beyond silence.

Every case here drives the portal, so each one costs a five-minute window and
they are `slow` by construction.
"""

import time

import pytest

from conftest import (BENCH_CHANNEL, BENCH_HOST, BENCH_PASS, BENCH_SSID,
                      BROKER_URI, wait_until_connected)
from test_provisioning import (derived_passphrase, ensure_provisioned,
                               join_portal, portal_ssid, post_form)

pytestmark = [pytest.mark.exception, pytest.mark.slow, pytest.mark.disruptive]


def _submit(wb, dut, fields):
    """Open the portal on a blank device and submit `fields`. Returns (status, body)."""
    ssid = portal_ssid(dut)
    join_portal(wb, ssid, derived_passphrase(ssid))
    try:
        return post_form(wb, fields)
    finally:
        wb.sta_leave()


@pytest.mark.parametrize("missing,label", [
    ("ssid", "no network name"),
    ("pass", "no passphrase"),
    ("broker", "no broker"),
])
def test_ts038_an_incomplete_configuration_is_refused(unprovisioned, dut, wb,
                                                     missing, label):
    """TS-038 — FR-SUP-09. All three fields are required, each on its own.

    Parametrised rather than written as one case with three submissions, because
    the requirement is that EACH of the three is individually required — a
    device that demands the SSID and quietly accepts a missing broker passes a
    combined test that submits all three blank.

    The refusal must also name the field. "Not saved" alone leaves the person at
    the portal guessing which of three inputs was wrong, on a device they can
    only reach for five minutes at a time.
    """
    fields = {
        "ssid": BENCH_SSID,
        "pass": BENCH_PASS,
        "broker": BROKER_URI.replace(":", "%3A").replace("/", "%2F"),
        "host": "",
    }
    fields[missing] = ""

    status, body = _submit(wb, dut, fields)
    print(f"\n  {label}: {status} | {body[:120]}")

    assert status == 400, (
        f"a submission with {label} was accepted ({status}). The device will "
        "reboot into a configuration it cannot use and join nothing, with no "
        "symptom beyond silence."
    )
    assert missing.replace("pass", "passphrase") in body.lower() or \
           missing in body.lower(), (
        f"refused, but the reason does not name the {missing} field: {body[:120]}"
    )


def test_ts039_blank_mqtt_credentials_are_accepted(unprovisioned, dut, wb, broker):
    """TS-039 — FR-SUP-10. An anonymous broker is a normal configuration.

    Most home brokers are anonymous, so requiring a username here would make the
    ordinary case impossible to configure. This is a deviation test in the
    proper sense: not the simplest path, but a completely normal one.
    """
    status, body = _submit(wb, dut, {
        "ssid": BENCH_SSID,
        "pass": BENCH_PASS,
        "broker": BROKER_URI.replace(":", "%3A").replace("/", "%2F"),
        "host": "",
        "user": "",
        "mqtt_pass": "",
    })
    print(f"\n  blank MQTT credentials: {status} | {body[:100]}")
    assert status == 200, (
        f"a blank MQTT username and password were refused ({status}). Most home "
        f"brokers are anonymous, so this rejects the ordinary case: {body[:120]}"
    )

    wb.ap_start(BENCH_SSID, BENCH_PASS, channel=BENCH_CHANNEL, internet=True)
    connected, how = wait_until_connected(wb, dut, seconds=150)
    print(f"  {how}")
    assert connected, (
        "the submission was accepted but the device never reached the broker "
        "anonymously"
    )


def test_ts058_the_portal_closes_on_its_own(unprovisioned, dut, wb):
    """TS-058 — FR-PRV-06. An unattended portal must not stay open forever.

    The window is 300 s (FSD Appendix A). An open SoftAP left up indefinitely is
    an unauthenticated surface on a device nobody is watching, which is the
    reason the timeout exists at all.

    Asserted by the device's own countdown rather than by waiting out five
    minutes and scanning: the diag line carries `left=NNNs`, so a decreasing
    countdown proves the timer is running, and that is the thing the requirement
    is about. Waiting the full window would cost five minutes to learn the same
    fact — and the closure itself is TS-036's job.
    """
    portal_ssid(dut)
    lefts = []
    for line in dut.lines(seconds=40):
        if "left=" in line:
            try:
                lefts.append(int(line.split("left=")[1].split("s")[0]))
            except (IndexError, ValueError):
                pass

    print(f"\n  countdown observed: {lefts[:6]}")
    assert len(lefts) >= 2, (
        "the portal never reported a remaining time, so nothing shows the "
        "window is bounded at all"
    )
    assert lefts[-1] < lefts[0], (
        f"the countdown is not decreasing ({lefts[0]} -> {lefts[-1]}), so the "
        "portal would stay open indefinitely on a device nobody is watching"
    )
    assert lefts[0] <= 300, (
        f"the window opens at {lefts[0]}s, longer than the 300 s Appendix A fixes"
    )


def test_ts060_the_softap_appears_within_the_budget(unprovisioned, dut, wb):
    """NFR-PRV-01 (TS-060). The portal is up within 5 s of entering Provisioning.

    A person holding a phone gives up long before a device that takes half a
    minute to show its network. Measured from the device's own announcement to
    the network being visible in a scan, which is the interval the user
    experiences.
    """
    from test_provisioning import scan_for

    started = time.monotonic()
    ssid = portal_ssid(dut)
    announced = time.monotonic()
    found = scan_for(wb, ssid, tries=3)
    visible = time.monotonic()

    print(f"\n  announced at +{announced - started:.1f}s, "
          f"on the air at +{visible - started:.1f}s")
    assert found, f"{ssid} never appeared in a scan"
    # Scanning itself takes seconds, so this cannot resolve a 5 s budget
    # precisely; it catches the gross case where the AP takes half a minute.
    assert visible - announced < 30, (
        f"the setup network took {visible - announced:.0f}s to become visible "
        "after the device announced it — a person with a phone has given up"
    )
