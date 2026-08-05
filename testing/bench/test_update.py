"""Bench tier — firmware update. Procedure P-OTA.

The relay is the workbench's own firmware store: POST /api/firmware/upload puts
an image on the Pi, and GET /firmware/<project>/<file> serves it as the URL the
board downloads from. Nothing had to be built for it, which is why these tests
run at all.

Rollback tests observe the *prohibited* outcome, not just recovery. A device
that comes back by rebooting into the same broken image has satisfied "did it
return" and failed the requirement.
"""

import re
import time

import pytest

PROJECT = "gplug-mini"


def image_url(wb_url, filename):
    """The URL as the board sees it — the Pi is 10.42.0.1 on the bench network,
    not the LAN address this test client uses."""
    return f"http://10.42.0.1:8080/firmware/{PROJECT}/{filename}"


@pytest.fixture
def relay(wb):
    """The workbench firmware store, and what it held before the test."""
    before = {f["filename"] for f in wb.firmware_list()}
    yield wb
    # Leave the rig as it was found: an image left in the store is one a later
    # test can download by accident.
    for f in wb.firmware_list():
        if f["filename"] not in before:
            try:
                wb.firmware_delete(f["project"], f["filename"])
            except Exception:
                pass


def publish_ota(broker_host, mac, url):
    """Publish a URL to the device's command topic (Appendix D).

    QoS 1, because an update command the broker drops is a command the person
    who sent it believes was delivered.
    """
    import paho.mqtt.publish as publish

    topic = f"gplug/{mac}/cmd/ota"
    publish.single(topic, url, qos=1, hostname=broker_host, port=1883)
    return topic


@pytest.mark.fast
@pytest.mark.disruptive
def test_ts062_a_url_on_the_command_topic_starts_a_download(dut, broker_host, relay, dut_mac):
    """TS-062 — FR-OTA-01. A URL on the command topic begins a download.

    Asserted from the device's own log rather than from the image landing,
    because the requirement is about the *trigger* — a download that begins and
    then fails for an unrelated reason has still satisfied this one.
    """
    mac = dut_mac
    dut.drain()
    publish_ota(broker_host, mac, image_url(None, "does-not-exist.bin"))

    line = dut.await_line(r"ota: downloading", seconds=15)
    print(f"\n  {line}")


@pytest.mark.fast
def test_ts062_neg_a_malformed_command_is_refused_and_said_so(dut, broker_host, relay, dut_mac):
    """TS-062 (negative) — FR-OTA-01, FR-OTA-02. A payload that is not a URL is
    refused, and the refusal is visible.

    An ignored command must not look like silence. A person who published a
    typo and sees nothing cannot tell it from a device that is not listening.
    """
    mac = dut_mac
    dut.drain()
    publish_ota(broker_host, mac, "10.42.0.1/app.bin")   # no scheme

    line = dut.await_line(r"not a URL this device will download from", seconds=15)
    print(f"\n  {line}")


@pytest.mark.slow
def test_ts063_nothing_triggers_a_download_but_the_command(dut, broker_host, relay):
    """TS-063 — FR-OTA-02. No download without a command.

    The declared test runs 24 h with a newer release published. This runs a
    bounded window with an image sitting in the relay unannounced, which is the
    same claim at the scale a suite can afford — and it is recorded as the
    weaker form rather than presented as the declared one.
    """
    relay.firmware_upload(PROJECT, __file__)   # any file; it must not be fetched
    dut.drain()
    lines = dut.lines(seconds=90)

    started = [line for line in lines if "ota: downloading" in line]
    assert not started, f"a download began with no command: {started}"
    print(f"\n  90 s with an unannounced image in the relay: no download")


@pytest.mark.slow
@pytest.mark.disruptive
def test_ts069_decoding_continues_during_a_download(dut, broker_host, sim, relay, dut_mac):
    """TS-069 — FR-OTA-08. The meter loop keeps reading through a download.

    A download that stalls ingestion drops several cycles of a household's
    consumption for an update nobody asked to lose them for. The assertion is
    that cycles keep being reported *while* the download is in flight.
    """
    mac = dut_mac
    dut.drain()
    publish_ota(broker_host, mac, image_url(None, "does-not-exist.bin"))

    lines = dut.lines(seconds=40)
    downloading = any("ota: downloading" in line for line in lines)
    cycles = [line for line in lines if "cycle:" in line]

    assert downloading, "the download never started, so this proves nothing about it"
    assert cycles, "no meter cycle was reported during the download window"
    print(f"\n  {len(cycles)} cycle(s) reported while a download was in flight")


@pytest.mark.fast
@pytest.mark.disruptive
def test_ts084_the_reset_reason_is_reported(dut):
    """TS-084 — FR-WDT-04. The reset reason is on the console after boot.

    Read after a deliberate reset, so the value is known in advance: a line that
    merely exists proves nothing, but one that says "software restart" after a
    software restart proves it is reading the register rather than printing a
    constant.
    """
    # Not read straight after the reset. SLOT1 is a native USB JTAG device, so a
    # reset re-enumerates the port and the first ~2.5 s of output goes with it —
    # measured, the earliest line any monitor sees is t=2647 ms, and the boot
    # report is long gone. The firmware repeats it once the network is up, which
    # is what this waits for.
    dut.drain()
    dut.reset()
    line = dut.await_line(r"reset reason:", seconds=60)
    print(f"\n  {line}")
    assert "power-on" not in line, (
        "a JTAG reset reported as power-on — the reason is not being read, "
        "and a watchdog reset would be indistinguishable from a power cut"
    )
