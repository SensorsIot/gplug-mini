"""Bench tier — updates, and the ways an update must be allowed to fail.

The valid-image fixture lives in the workbench firmware store and is served on
the AP-side address the device itself uses; the plan's `ota-relay` capability
carries the URL. The three fixtures that do not exist yet — an image that boots
but cannot reach the broker, and endpoints presenting a self-signed and an
expired certificate — are not files that can be copied, so the cases needing
them stay in the plan with their contracts intact and are skipped here with the
reason rather than quietly omitted.

Every case in this file is disruptive: an update reboots the device.
"""

import time

import pytest

from conftest import BENCH_AP_GATEWAY, BENCH_HOST, MqttWatch

pytestmark = [pytest.mark.exception, pytest.mark.slow, pytest.mark.disruptive]

PROJECT = "gplug-mini"
VALID_IMAGE = f"http://{BENCH_AP_GATEWAY}:8080/firmware/{PROJECT}/gplug-mini-valid.bin"

# WARNING, and it is not a small one: a successful update REPLACES the firmware
# under test. Every case after TS-062 runs against whatever image the relay
# serves, not the build the run set out to verify — and the relay's image is
# whatever somebody uploaded last.
#
# That silently invalidated a whole investigation: the relay held a build from
# four hours before a discovery fix, so the device spent every run after the OTA
# phase without it, and the resulting failures read as a firmware defect that had
# in fact been fixed. Two experiments were run against the wrong binary before
# anyone compared the file sizes.
#
# So the relay image must BE the build under test. Upload it before the run:
#
#     curl -F project=gplug-mini -F file=@build/gplug-mini.bin \
#          -F filename=gplug-mini-valid.bin $WT/api/firmware/upload
#
# and re-flash the device afterwards if these cases ran. Until the harness owns
# that, treat any result taken after this file as suspect.
STATUS = "gplug/+/status"
STATE = "gplug/+/state"


def publish_ota(mac, url):
    """The command topic carries a URL and nothing else (Appendix D)."""
    import paho.mqtt.publish as publish

    topic = f"gplug/{mac}/cmd/ota"
    publish.single(topic, url, qos=1, hostname=BENCH_HOST, port=1883)
    return topic


def _uptime_ms(dut, seconds=25, tries=3):
    """The device's own uptime from any log line, which is how a reboot is seen.

    Reading the uptime is what distinguishes "it restarted" from "it went quiet":
    the console drops output for long stretches on this board, so absence proves
    nothing, but a timestamp that went backwards is unambiguous.

    Retried, because that same silence can swallow a whole 25 s window. A single
    empty read reports a running board as "silent to begin with" and fails the
    test before it has begun — which is a statement about the console, not about
    the device.
    """
    import re
    for _ in range(tries):
        best = None
        for line in dut.lines(seconds=seconds):
            m = re.search(r"^\s*[IWE]\s*\((\d+)\)", line)
            if m:
                best = int(m.group(1))
        if best is not None:
            return best
    return None


def test_ts062_a_url_on_the_command_topic_starts_an_update(dut, broker, sim, dut_mac):
    """TS-062 — FR-OTA-01. The only trigger there is.

    Proven by the device RESTARTING into the new image, not by a log line saying
    it started downloading. That line is emitted once and this board's console
    loses it often enough that a test hung on it reports a working update as a
    failure while the uptime plainly shows the device restarted.
    """
    before = _uptime_ms(dut)
    assert before is not None, "the device is printing nothing at all to begin with"
    print(f"\n  uptime before: {before} ms")

    publish_ota(dut_mac, VALID_IMAGE)

    deadline = time.monotonic() + 240
    after = None
    while time.monotonic() < deadline:
        after = _uptime_ms(dut, seconds=20)
        if after is not None and after < before:
            break
    print(f"  uptime after:  {after} ms")

    assert after is not None and after < before, (
        f"the device never restarted after a valid image URL was published to "
        f"{dut_mac}'s command topic (uptime {before} -> {after}). FR-OTA-01 is "
        "the only update trigger this product has, so an update that cannot be "
        "commanded cannot be delivered at all."
    )


def test_ts069_decoding_continues_during_an_update(dut, broker, sim, dut_mac):
    """TS-069 — FR-OTA-08. The meter does not stop because a download started.

    A device that stops decoding while it downloads loses the counters it misses,
    and the meter does not replay them. Asserted across the download window
    rather than after it, because afterwards the device has rebooted and the
    question is unanswerable.
    """
    dut.drain()
    publish_ota(dut_mac, VALID_IMAGE)

    cycles = []
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        for line in dut.lines(seconds=10):
            if "cycle:" in line:
                cycles.append(line)
        if len(cycles) >= 3:
            break

    print(f"\n  {len(cycles)} meter cycle(s) reported during the download")
    assert cycles, (
        "the device reported no meter cycle at all while downloading. Whatever "
        "the meter sent during the update is gone — it does not replay."
    )


def test_ts062_neg_a_malformed_command_is_refused(dut, broker, sim, dut_mac):
    """TS-062 (negative) — FR-OTA-01. Junk on the command topic changes nothing.

    The prohibited outcome is the assertion: the device must NOT restart. A
    refusal that is logged but still reboots the device has failed the
    requirement in the way that matters.
    """
    before = _uptime_ms(dut)
    assert before is not None, "the device is silent to begin with"

    publish_ota(dut_mac, "not-a-url")
    time.sleep(45)
    after = _uptime_ms(dut)

    print(f"\n  uptime {before} -> {after} ms after a malformed command")
    assert after is not None and after > before, (
        f"the device restarted after junk was published to its command topic "
        f"(uptime {before} -> {after}). Anything able to publish to the broker "
        "could then reboot it at will."
    )


def test_ts063_nothing_but_the_command_triggers_an_update(dut, broker, sim, dut_mac):
    """TS-063 — FR-OTA-02. No polling, no schedule, no version check.

    An image sitting in the relay, unannounced, must be ignored. Automatic
    polling is what delivers a bad build everywhere before anyone notices
    (D-U2), and the absence of it cannot be read from the source — only from a
    device that had every opportunity and did nothing.
    """
    before = _uptime_ms(dut)
    assert before is not None, "the device is silent to begin with"

    # The image is present and served; nothing announces it.
    print(f"\n  {VALID_IMAGE} is in the relay, unannounced")
    time.sleep(90)
    after = _uptime_ms(dut)

    print(f"  uptime {before} -> {after} ms after 90 s with an unannounced image")
    assert after is not None and after > before, (
        f"the device restarted with no command published (uptime {before} -> "
        f"{after}) — it is finding updates by itself, which is exactly what "
        "FR-OTA-02 forbids"
    )


@pytest.mark.skip(reason="needs the broker-incapable image fixture, which has to "
                         "be built rather than copied — ota-relay fixtures in "
                         "test-plan.yaml")
def test_ts066_an_image_that_cannot_reach_the_broker_is_rolled_back(dut, broker):
    """TS-066 — FR-OTA-05. The case rollback exists for.

    A session, not a successful boot, is what makes an image valid: an image
    that boots perfectly and cannot reach the broker is precisely the one that
    must be reverted. Needs a deliberately broker-incapable build.
    """


@pytest.mark.skip(reason="needs a TLS endpoint with a self-signed certificate — "
                         "ota-relay fixtures in test-plan.yaml")
def test_ts089_a_self_signed_certificate_aborts_the_download(dut, broker):
    """TS-089 — FR-SEC-01. An untrusted certificate must stop the update."""


@pytest.mark.skip(reason="needs a TLS endpoint with an expired certificate — "
                         "ota-relay fixtures in test-plan.yaml")
def test_ts090_an_expired_certificate_aborts_the_download(dut, broker):
    """TS-090 — FR-SEC-02. An expired certificate must stop the update."""
