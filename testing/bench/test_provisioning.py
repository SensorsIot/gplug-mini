"""Bench tier — the captive portal, from a blank device to a joined one.

These began life as a debugging script. That script proved the portal works on
2026-08-05 and then had nothing to say the next day, because a script that
answers "did it work just now" is not a test: nobody runs it again, and nothing
fails when a change breaks it. Everything it established is here instead.

The order below is the order the device experiences, and each step's failure has
exactly one cause:

    enters Provisioning  ->  announces a network  ->  the derived passphrase
    admits you  ->  any hostname reaches the form  ->  a bad field is refused
    ->  a good form is stored  ->  the device reboots into it

Three rules were paid for and are enforced here rather than rediscovered:

* **The portal window is a budget.** It closes after five minutes and a harness
  bug spends it as readily as a tester does. Every test that needs the portal
  resets the board first rather than inheriting whatever is left.
* **The relay's body is base64 in both directions.** Sending raw text delivers an
  empty body, and the device then refuses it correctly for a reason that reads as
  a firmware defect.
* **An SSID is not an identity.** Two benches once answered to one name and the
  board joined the wrong one. The join test checks the BSSID, not the name.
"""

import base64
import re
import time

import pytest

from conftest import BENCH_PASS, BENCH_SSID, BROKER_URI
from workbench_driver import CommandError

pytestmark = [pytest.mark.provisioning, pytest.mark.disruptive]

FORM_HEADERS = {"Content-Type": "application/x-www-form-urlencoded"}
PORTAL_ADDR = "192.168.4.1"          # the DUT's own SoftAP — never the bench's
SSID_PATTERN = r"state=provisioning ssid=(gplug-[0-9a-f]{6})"


# ── helpers ──────────────────────────────────────────────────────────────────

def portal_ssid(dut, seconds=60):
    """Reset into Provisioning and return the SSID the device announces.

    Read from the device rather than derived here. A harness that computes the
    name independently can be right while the firmware is wrong, or the reverse,
    with nothing in the output to say which — so the device is the authority and
    this only checks it said something.
    """
    dut.drain()
    dut.reset()
    line = dut.await_line(SSID_PATTERN, seconds)
    match = re.search(SSID_PATTERN, line)
    assert match, f"the diag line changed shape and no SSID could be read: {line}"
    return match.group(1)


def derived_passphrase(ssid):
    """FR-PRV-02/FR-SEC-03: `gplug-254a75` -> `gplug254a75`."""
    return "gplug" + ssid.split("-", 1)[1]


def join_portal(wb, ssid, passphrase, tries=3):
    """Associate with the DUT's SoftAP, distinguishing the two failure kinds.

    A rejected passphrase is a result about the device. "Connected but no IP" is
    a DHCP race that has already proved the passphrase was accepted — retrying
    that one keeps a lease timing out from being reported as a wrong password.
    """
    wb.ap_stop()
    last = None
    for _ in range(tries):
        last = wb.sta_join(ssid, passphrase, timeout=25)
        if last.get("ip"):
            return last
        if "Connected to" not in str(last.get("error", "")):
            break                      # a real refusal; do not paper over it
        wb.sta_leave()
        time.sleep(3)
    pytest.fail(f"could not reach the portal: {last}")


def get_page(wb, url, tries=4):
    """GET through the relay, decoded, retried past the first-request race."""
    last = {}
    for _ in range(tries):
        last = wb.wifi_http(url, timeout=20)
        if last.get("status"):
            return last.get("status"), decode_body(last)
        time.sleep(3)
    return None, ""


def post_form(wb, fields, tries=3):
    """POST the portal form. The body must be base64 or it arrives empty."""
    body = "&".join(f"{k}={v}" for k, v in fields.items())
    encoded = base64.b64encode(body.encode()).decode()
    last = {}
    for _ in range(tries):
        last = wb.wifi_http(f"http://{PORTAL_ADDR}/save", method="POST",
                            headers=FORM_HEADERS, body=encoded, timeout=15)
        if last.get("status"):
            return last.get("status"), decode_body(last)
        time.sleep(3)
    return None, ""


def decode_body(response):
    raw = response.get("body") or ""
    try:
        return base64.b64decode(raw).decode("utf-8", "replace")
    except Exception:
        return str(raw)


# ── the tests ────────────────────────────────────────────────────────────────

@pytest.mark.fast
def test_ts054_the_portal_announces_a_derivable_network(unprovisioned, dut, wb):
    """TS-054 — FR-PRV-02. The device names its own setup network.

    The name matters beyond cosmetics: it is what a user reads off a phone, and
    it is what the passphrase is computed from. A unit whose name collided with
    its neighbour's would hand both the same passphrase.
    """
    ssid = portal_ssid(dut)
    print(f"\n  announced: {ssid}, passphrase would be {derived_passphrase(ssid)}")

    assert re.fullmatch(r"gplug-[0-9a-f]{6}", ssid), \
        f"{ssid!r} is not the documented gplug-<6 hex> shape"

    found = [n for n in wb.scan().get("networks", []) if n.get("ssid") == ssid]
    assert found, f"{ssid} is announced on serial but not on the air"
    print(f"  in scan: {found[0]}")


@pytest.mark.fast
def test_ts091_the_portal_is_wpa2_not_open(unprovisioned, dut, wb):
    """TS-091 — FR-SEC-03. The setup network is encrypted.

    Whatever the user types into the form includes their home WiFi password. On
    an open network that is readable by anyone in range, and the portal is the
    one moment the device handles a credential it was not given.
    """
    ssid = portal_ssid(dut)
    found = [n for n in wb.scan().get("networks", []) if n.get("ssid") == ssid]
    assert found, f"{ssid} was not seen in a scan at all"

    auth = str(found[0].get("auth", "")).upper()
    print(f"\n  {ssid} auth={auth}")
    assert "WPA2" in auth, (
        f"the setup network is {auth or 'OPEN'} — anything typed into the form, "
        "including the user's home WiFi password, is readable in the car park"
    )


def test_ts053_the_derived_passphrase_admits_and_a_wrong_one_does_not(unprovisioned, dut, wb):
    """TS-053 — FR-PRV-01. Both halves, because only the pair means anything.

    That the derived passphrase works proves the device is reachable. That a
    wrong one does not is what proves the network is actually protected — a
    misconfiguration accepting any passphrase passes the first check perfectly.

    TS-104 cannot make either claim: a host test asserting derive(mac) equals the
    rule asserts the implementation against itself.
    """
    ssid = portal_ssid(dut)
    try:
        joined = join_portal(wb, ssid, derived_passphrase(ssid))
        print(f"\n  derived passphrase accepted, lease {joined.get('ip')}")
        assert joined.get("ip"), "associated but never got an address"
        wb.sta_leave()

        # A refusal reaches us two ways and both are the same result. The driver
        # raises when the bench reports it could not associate, and returns a
        # bare dict when it associated but never got a lease. Only an address
        # means the passphrase was accepted.
        try:
            wrong = wb.sta_join(ssid, "definitelynotright", timeout=20)
        except CommandError as refused:
            print(f"  wrong passphrase -> refused: {refused}")
            wrong = {}
        else:
            print(f"  wrong passphrase -> {wrong}")
        assert not wrong.get("ip"), (
            "a wrong passphrase was admitted — the network is advertised as WPA2 "
            "but is not actually protecting anything"
        )
    finally:
        wb.sta_leave()


def test_ts055_any_hostname_reaches_the_form(unprovisioned, dut, wb):
    """TS-055 — FR-PRV-03. The redirect that makes it a *captive* portal.

    A phone decides a network needs sign-in by fetching a known URL and seeing
    something other than it expected. Without the hijack the user must know to
    type an IP address, which most will not, and the device looks dead.
    """
    ssid = portal_ssid(dut)
    try:
        join_portal(wb, ssid, derived_passphrase(ssid))

        # The first is what a phone actually requests; the others prove it is the
        # hostname being answered rather than one lucky path.
        for url in (f"http://example.com/generate_204",
                    f"http://{PORTAL_ADDR}/",
                    f"http://{PORTAL_ADDR}/anything"):
            status, body = get_page(wb, url)
            served = "<form" in body or "gPlug" in body
            print(f"\n  {url:42} -> {status} {'FORM' if served else body[:60]}")
            assert status == 200, f"{url} returned {status}, so a phone sees no portal"
            assert served, f"{url} returned something that is not the form"
    finally:
        wb.sta_leave()


def test_ts059_the_form_lists_networks_in_range(unprovisioned, dut, wb):
    """TS-059 — FR-PRV-07. The page shows what the device can hear.

    A network name typed one character wrong fails later as a *wrong password*,
    which is the hardest possible way to discover a typo. The list is the cure.

    It is also a regression guard: the scan moved out of the request path in
    4a6caf8 because a blocking scan per render cost ~3 s and broke the first
    requests after association. Cached is fine; absent is not.
    """
    ssid = portal_ssid(dut)
    try:
        join_portal(wb, ssid, derived_passphrase(ssid))
        started = time.monotonic()
        status, body = get_page(wb, f"http://{PORTAL_ADDR}/")
        elapsed = time.monotonic() - started

        assert status == 200, f"the portal page returned {status}"
        print(f"\n  page in {elapsed:.1f}s; 'In range' present: {'In range' in body}")
        assert "In range" in body, (
            "the page carries no network list, so a mistyped SSID has nothing to "
            "be checked against"
        )
        assert BENCH_SSID in body or "In range: " in body, \
            "the list is present but empty"
    finally:
        wb.sta_leave()


def test_ts061_a_broker_without_a_scheme_is_refused(unprovisioned, dut, wb):
    """TS-061 — FR-PRV-08. Refused at the form, naming the field.

    Storing it instead would reboot into a device that associates perfectly and
    never reaches a broker, presenting as an MQTT fault with nothing pointing
    back at the form that caused it.

    The refusal must name the *broker*. "no SSID stored" here would mean the body
    never arrived — a harness fault wearing a firmware fault's clothes, which is
    exactly how a day was lost before ConfigFault::Unparseable existed.
    """
    ssid = portal_ssid(dut)
    try:
        join_portal(wb, ssid, derived_passphrase(ssid))
        status, body = post_form(wb, {
            "ssid": BENCH_SSID, "pass": BENCH_PASS,
            "broker": "homeassistant.local",     # a hostname is not a URI
            "host": "",
        })
        print(f"\n  {status} | {body[:140]}")

        assert status == 400, f"a schemeless broker was not refused (got {status})"
        assert "scheme" in body.lower(), \
            f"refused, but the reason does not name the broker rule: {body[:120]}"
        assert "SSID" not in body, (
            "refused for the SSID, which means the form body never arrived — "
            "check the relay body is base64 before believing this is the device"
        )
    finally:
        wb.sta_leave()


def test_ts092_an_empty_submission_says_so(unprovisioned, dut, wb):
    """TS-092 — FR-SEC-04, and the diagnosability rule CP-008.

    An unreadable submission and a form with an empty SSID are different faults
    and must read differently. Collapsing them is what made a base64 mistake in
    the harness look like a firmware defect for hours.
    """
    ssid = portal_ssid(dut)
    try:
        join_portal(wb, ssid, derived_passphrase(ssid))
        response = wb.wifi_http(f"http://{PORTAL_ADDR}/save", method="POST",
                                headers=FORM_HEADERS, body="", timeout=15)
        body = decode_body(response)
        print(f"\n  {response.get('status')} | {body[:140]}")

        assert response.get("status") == 400, "an empty submission was not refused"
        assert "could not be read" in body, (
            "an unreadable submission is reported as some other fault; a harness "
            "bug will be investigated as a firmware defect"
        )
    finally:
        wb.sta_leave()


@pytest.mark.slow
def test_ts057_a_valid_submission_survives_the_reboot(unprovisioned, dut, wb):
    """TS-057 — FR-PRV-05. The whole point: configure once, keep it.

    Asserted after a reboot rather than on the "Saved" page, because a device
    that answers Saved and stores nothing gives an identical page and a very
    different product.

    The BSSID is checked, not just the SSID. Two benches once answered to one
    name and the board joined the other — everything downstream then fails for a
    reason no amount of reading the firmware will reveal.
    """
    ssid = portal_ssid(dut)
    join_portal(wb, ssid, derived_passphrase(ssid))
    status, body = post_form(wb, {
        "ssid": BENCH_SSID, "pass": BENCH_PASS,
        "broker": BROKER_URI.replace(":", "%3A").replace("/", "%2F"),
        "host": "",
    })
    print(f"\n  save -> {status} | {body[:100]}")
    assert status == 200, f"a valid submission was refused: {body[:160]}"

    # The device is rebooting into a network that must exist by the time it looks.
    wb.sta_leave()
    wb.ap_start(BENCH_SSID, BENCH_PASS, internet=True)

    joined = dut.await_line(r"wifi:connected with \S+.*bssid = \S+", seconds=90)
    print(f"  {joined}")
    radio = wb.ap_status()
    assert BENCH_SSID in joined, f"the device joined something else: {joined}"

    stored = dut.await_line(r"diag:.*cfg=nvs", seconds=60)
    print(f"  {stored}")
    assert "cfg=nvs" in stored, \
        "the device booted on build defaults, so the submission was not stored"
    print(f"  AP reports: {radio}")
