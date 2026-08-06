"""WF-001 — commission a factory-new device. One continuous execution.

This is the journey the product exists to perform, and the whole reason the plan
has a second kind of test case. Its atomic children all pass today; that is not
the same claim. Each of them proves one contract in isolation, with the rig
arranged to suit it, and none of them proves that what one step produces is what
the next step consumes. WF-001 makes that handoff the thing under test:

    blank NVS -> portal -> submitted form -> the configured network
              -> a broker session -> a decoded meter identity
              -> retained discovery -> live measurements on the topic
                                       those configs point at

So it is deliberately *not* a loop over its children. `children` in the plan is
traceability; the steps below are one execution, and the workflow can fail with
every child green. That is the result worth being able to state.

Three things it asserts that no atomic case does:

* **Discovery waits for the meter identity** (FR-HA-03), observed as an ordering
  rather than as a state: the broker is cleared, the simulator is put on
  `identity none`, and the absence of a config is checked *before* the identity
  is restored. A test that only looks at the end cannot tell "published after"
  from "published all along".
* **The state topic in the discovery payload is the topic being published to.**
  Both halves pass their own tests while disagreeing with each other, and the
  product is then a set of entities that never update.
* **The BSSID, not the SSID.** Two benches once answered to one name.

Run:  pytest testing/bench/test_wf001_commissioning.py --wt-url http://<bench>:8080
"""

import base64
import json
import os
import re
import time

import pytest

from conftest import (
    BENCH_HOST, BENCH_PASS, BENCH_SSID, BROKER_URI, MqttWatch,
)
from test_provisioning import (
    FORM_HEADERS, PORTAL_ADDR, derived_passphrase, ensure_provisioned, get_page,
    join_portal, portal_ssid, post_form,
)

# Phase 0: this is where a device's life starts, and everything downstream reads
# as a defect if it is broken. Disruptive because it blanks NVS and reboots.
pytestmark = [pytest.mark.provisioning, pytest.mark.workflow,
              pytest.mark.slow, pytest.mark.disruptive]

DISCOVERY_WILDCARD = "homeassistant/sensor/+/config"
STATE_WILDCARD = "gplug/+/state"
STATUS_WILDCARD = "gplug/+/status"

# Appendix B, the entities the mode-2 cycle actually carries. Mode 2 delivers
# power, energy and the identity — not the per-phase voltages and currents — so
# requiring all of Appendix B here would fail on a rig characteristic. What must
# hold is the other direction: nothing may be published that the cycle did not
# contain (FR-HA-05).
EXPECTED_FROM_MODE_2 = {"Ei", "Pi"}
CLASSES = {
    "energy": ("total_increasing", "kWh"),
    "power": ("measurement", "kW"),
}

# How long the device is given at each handoff. Every one is a budget, not a
# guess about how long the step takes: the portal window is five minutes total
# and a workflow that spends it waiting has failed for its own reasons.
JOIN_TIMEOUT = 90
SESSION_TIMEOUT = 90
DEFERRAL_WINDOW = 30      # long enough that "not yet" cannot be "not looked"
DISCOVERY_TIMEOUT = 90
STATE_TIMEOUT = 90


def _bssid_suffix(ssid):
    """`wb-7cb1c2` -> `7c:b1:c2`, the last three octets the SSID was built from.

    The SSID carries the bench radio's own MAC precisely so this check needs no
    second source. Deriving it from a written-down address would reintroduce the
    thing that cost a day: an address that is correct and a radio that is not the
    one being talked to.
    """
    hexes = ssid.split("-")[-1]
    return ":".join(hexes[i:i + 2] for i in range(0, 6, 2))


def _payloads_by_label(configs):
    """Discovery configs keyed by the entity label at the end of the topic."""
    out = {}
    for _t, topic, payload, _r in configs:
        label = topic.split("/")[-2].split("_")[-1]
        try:
            out[label] = json.loads(payload)
        except json.JSONDecodeError:
            pytest.fail(f"{topic} carries a payload that is not JSON: {payload[:120]}")
    return out


def _write_evidence(evidence):
    """The evidence contract WF-001 declares, written where a later run can read it."""
    run = time.strftime("%Y%m%dT%H%M%S")
    path = os.path.join(
        os.path.dirname(__file__), "..", "..", "test-results", run, "WF-001.json"
    )
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(evidence, f, indent=2, default=str)
    return path


def test_wf001_commission_a_factory_new_device(wb, dut, sim, broker, unprovisioned):
    """WF-001 — AC-1. A factory-new device becomes a publishing one, unattended.

    Checkpoints, in order, each carrying the atomic contract it exercises:

        TS-030  boots into Provisioning with no stored configuration
        TS-053/054/060  announces a WPA2 network whose passphrase derives
        TS-055/059  any hostname reaches a form listing networks in range
        TS-057  a valid submission survives the reboot
        TS-031  joins the configured network — checked by BSSID
        TS-108/109  the cycle decodes and the meter identity is read
        TS-045/111  discovery is retained, and only after the identity
        TS-110  live state arrives on the topic the configs point at
        TS-071  the payload reports the running firmware version

    Fails as a whole if any handoff between them does not hold.
    """
    evidence = {"workflow": "WF-001", "validates": "AC-1", "steps": []}
    watch = MqttWatch(BENCH_HOST, [DISCOVERY_WILDCARD, STATE_WILDCARD, STATUS_WILDCARD])

    def step(name, detail=""):
        print(f"\n=== {name} {detail}")
        evidence["steps"].append({"step": name, "detail": detail, "at": time.time()})

    try:
        wb.test_start("WF-001 commission a factory-new device", "provisioning", 9)

        # ── 1. the meter says nothing identifying itself ────────────────────
        #
        # Before the device is even configured, so the deferral assertion later
        # is about ordering rather than about what the rig happened to be doing.
        step("arrange", "simulator on identity none, retained discovery cleared")
        sim.command("identity none")
        state = sim.status()
        assert state.get("identity") == "none", \
            f"simulator would not drop its identity: {state}"

        stale = [m[1] for m in watch.messages if m[1].startswith("homeassistant/")]
        watch.clear_retained(stale)
        evidence["cleared_retained_configs"] = stale
        print(f"  cleared {len(stale)} retained config(s) from a previous run")

        # ── 2. a device with nothing stored raises its portal ───────────────
        wb.test_step("TS-030", "factory boot", "blank NVS, expect Provisioning")
        step("factory boot", "NVS blanked by the fixture")
        ssid = portal_ssid(dut)
        passphrase = derived_passphrase(ssid)
        evidence["portal_ssid"] = ssid
        print(f"  portal: {ssid}, derived passphrase {passphrase}")

        seen = [n for n in wb.scan().get("networks", []) if n.get("ssid") == ssid]
        assert seen, f"{ssid} is on the serial log but not on the air"
        auth = str(seen[0].get("auth", "")).upper()
        evidence["portal_auth"] = auth
        assert "WPA2" in auth, (
            f"the setup network is {auth or 'OPEN'} — the form carries the user's "
            "home WiFi password, so this is the one moment it must be encrypted"
        )

        # ── 3. the portal admits us and serves the form ─────────────────────
        wb.test_step("TS-055", "captive portal", "join the SoftAP, request a page")
        step("open the portal")
        joined = join_portal(wb, ssid, passphrase)
        evidence["portal_lease"] = joined.get("ip")
        print(f"  joined, lease {joined.get('ip')}")

        status, body = get_page(wb, "http://example.com/generate_204")
        assert status == 200 and ("<form" in body or "gPlug" in body), (
            f"a phone's connectivity check got {status} and no form, so the "
            "portal never opens by itself"
        )
        assert "In range" in body, \
            "the form lists no networks, so a mistyped SSID has nothing to check against"
        print(f"  form served to a foreign hostname, {len(body)} bytes, network list present")

        # ── 4. configure it, and prove the configuration survived ───────────
        wb.test_step("TS-057", "submit configuration", "then reboot into it")
        step("submit the form", f"ssid={BENCH_SSID} broker={BROKER_URI}")
        status, body = post_form(wb, {
            "ssid": BENCH_SSID, "pass": BENCH_PASS,
            "broker": BROKER_URI.replace(":", "%3A").replace("/", "%2F"),
            "host": "",
        })
        evidence["save_status"] = status
        assert status == 200, f"a valid submission was refused: {body[:200]}"
        print(f"  saved: {status}")

        # The bench radio was a client of the DUT; it must now be the network the
        # DUT was told to join. It cannot be both — one radio.
        wb.sta_leave()
        wb.ap_start(BENCH_SSID, BENCH_PASS, internet=True)

        # ── 5. it joins the network it was given, not one with the same name ─
        wb.test_step("TS-031", "join configured AP", "the device appears on THIS bench's AP")
        step("join the configured network")

        # Watch the access point, not the device's log. The board announces its
        # association once, early, in a line that has usually scrolled past
        # before a workflow gets to look — TS-057 catches it only because it
        # starts watching a moment sooner, and the same line arrived here with a
        # corrupted character in it. The station list is better evidence anyway:
        # a device that appears on OUR radio has demonstrably joined OUR network,
        # which is the thing the BSSID check was a proxy for.
        deadline = time.monotonic() + JOIN_TIMEOUT
        stations = []
        while time.monotonic() < deadline:
            stations = (wb.ap_status() or {}).get("stations") or []
            if stations:
                break
            time.sleep(3)
        evidence["stations"] = stations
        assert stations, (
            f"no station joined {BENCH_SSID} within {JOIN_TIMEOUT}s. The device "
            "may be associating with another access point of the same name, or "
            "failing to get a lease — check the bench's own `iw dev wlan0 "
            "station dump`, because ap_status has been seen to lag it."
        )
        dut_mac = stations[0]["mac"]
        print(f"  station {dut_mac} at {stations[0].get('ip')} on {BENCH_SSID}")

        # The BSSID line if it is still in the buffer — recorded, not required.
        for line in dut.lines(seconds=5):
            found = re.search(r"bssid = ([0-9a-f:]{17})", line)
            if found:
                evidence["bssid"] = found.group(1)
                expected = _bssid_suffix(BENCH_SSID)
                assert found.group(1).endswith(expected), (
                    f"associated with {found.group(1)}, which is not this bench "
                    f"(expected an address ending {expected})"
                )
                print(f"  {line}")
                break

        stored = dut.await_line(r"diag:.*cfg=nvs", seconds=60)
        assert "cfg=nvs" in stored, \
            "the device booted on build defaults — the submission was not stored"
        print(f"  {stored}")

        # ── 6. a broker session, and no discovery while the meter is anonymous ─
        wb.test_step("TS-045", "discovery deferral", f"{DEFERRAL_WINDOW}s with no identity")
        step("broker session", "meter still on identity none")
        # `broker=up` in the diag line, because that is what the firmware
        # actually prints. There is no "mqtt connected" message: net.cpp logs
        # only on error, and the session state reaches the console through the
        # periodic diag line. Waiting 90 s for a phrase the device never emits
        # is how this step failed while the board sat there publishing.
        dut.await_line(r"diag:.*broker=up", seconds=SESSION_TIMEOUT)

        mark = watch.mark()
        time.sleep(DEFERRAL_WINDOW)
        premature = watch.since(mark, "homeassistant/")
        evidence["configs_while_anonymous"] = [m[1] for m in premature]
        print(f"  {DEFERRAL_WINDOW}s with an anonymous meter: "
              f"{len(premature)} discovery config(s)")
        assert not premature, (
            "discovery was published before the meter identity was known "
            f"({[m[1] for m in premature][:3]}). The entities are then keyed on a "
            "placeholder, and the real serial creates a second set that splits the "
            "household's history in two."
        )

        # ── 7. give it an identity; discovery must follow ───────────────────
        wb.test_step("TS-109", "meter identity", "restore identity, expect discovery")
        step("restore the meter identity")
        sim.command("identity ldn")
        assert sim.status().get("identity") != "none", "the simulator kept its identity off"

        mark = watch.mark()
        deadline = time.monotonic() + DISCOVERY_TIMEOUT
        while time.monotonic() < deadline and len(watch.since(mark, "homeassistant/")) < 2:
            time.sleep(2)
        configs = watch.since(mark, "homeassistant/")
        evidence["discovery_topics"] = [m[1] for m in configs]
        print(f"  {len(configs)} discovery config(s) after the identity arrived")
        for _t, topic, _p, retain in configs[:6]:
            print(f"    {topic}  retain={retain}")

        assert configs, (
            f"no discovery config in {DISCOVERY_TIMEOUT}s after the meter identified "
            "itself. The device decodes and connects, and a consumer sees nothing."
        )
        assert all(retain for _t, _top, _p, retain in configs), (
            "a discovery config was published without the retain flag, so anything "
            "subscribing later learns no entities at all"
        )

        # ── 8. the payloads carry what a consumer needs ─────────────────────
        wb.test_step("TS-111", "discovery contract", "classes, units, identity, version")
        step("check the discovery contract")
        by_label = _payloads_by_label(configs)
        evidence["entities"] = sorted(by_label)
        print(f"  entities: {sorted(by_label)}")

        missing = EXPECTED_FROM_MODE_2 - set(by_label)
        assert not missing, (
            f"the cycle carried {sorted(EXPECTED_FROM_MODE_2)} but no configuration "
            f"was published for {sorted(missing)}"
        )

        serial = None
        for label, payload in by_label.items():
            uid = payload.get("unique_id", "")
            assert uid.endswith(f"_{label}"), \
                f"{label}: unique_id {uid!r} is not keyed on the meter serial"
            serial = serial or uid.rsplit("_", 1)[0]

            device_class = payload.get("device_class")
            assert device_class in CLASSES, \
                f"{label}: device_class {device_class!r} is not one this product publishes"
            state_class, unit = CLASSES[device_class]
            assert payload.get("state_class") == state_class, (
                f"{label}: {device_class} entity carries state_class "
                f"{payload.get('state_class')!r}, not {state_class!r} — an energy "
                "register that is not total_increasing cannot become consumption"
            )
            assert payload.get("unit_of_measurement") == unit, \
                f"{label}: unit is {payload.get('unit_of_measurement')!r}, not {unit!r}"
        evidence["meter_serial"] = serial
        print(f"  all entities keyed on serial {serial}, classes and units correct")

        # ── 9. live measurements, on the topic the configs point at ─────────
        wb.test_step("TS-110", "live publication", "two fresh messages")
        step("live publication")
        mark = watch.mark()
        deadline = time.monotonic() + STATE_TIMEOUT
        while time.monotonic() < deadline and len(watch.since(mark, "gplug/")) < 3:
            time.sleep(2)
        fresh = [m for m in watch.since(mark, "gplug/") if m[1].endswith("/state")]
        evidence["state_messages"] = len(fresh)
        print(f"  {len(fresh)} state message(s) in {STATE_TIMEOUT}s")

        assert len(fresh) >= 2, (
            f"only {len(fresh)} state message(s) arrived. One may be a retained copy "
            "from before this step; two arrivals are what prove the device is "
            "publishing now rather than having published once."
        )
        assert not any(retain for _t, _top, _p, retain in fresh), \
            "a state message was retained — FR-HA-07 forbids it, because a stale " \
            "reading is indistinguishable from a real one of the same value"

        published_topic = fresh[-1][1]
        declared = {p.get("state_topic") for p in by_label.values()}
        evidence["state_topic_published"] = published_topic
        evidence["state_topic_declared"] = sorted(t for t in declared if t)
        assert declared == {published_topic}, (
            f"the discovery configs point at {sorted(declared)} and the device "
            f"publishes to {published_topic}. Both halves pass their own tests and "
            "the product is a set of entities that never update."
        )

        payload = json.loads(fresh[-1][2])
        for label in by_label:
            assert label in payload, (
                f"an entity was declared for {label} but the state message does not "
                f"carry it: {sorted(payload)}. FR-HA-05 — nothing may be published "
                "that the cycle did not contain, in either direction."
            )
        print(f"  {published_topic}  {fresh[-1][2][:120]}")

        # NFR-OTA-01. Last, because it is the one thing here that is about the
        # image rather than the meter, and a failure should not hide the journey.
        wb.test_step("TS-071", "reported version", "device.sw_version")
        versions = {label: (p.get("device") or {}).get("sw_version")
                    for label, p in by_label.items()}
        evidence["sw_version"] = versions
        assert all(versions.values()), (
            f"the device block reports no sw_version ({versions}). NFR-OTA-01 makes "
            "this a Must: after an OTA there is no other way to tell which image is "
            "running from outside the device."
        )

        wb.test_result("WF-001", "commission a factory-new device", "PASS")

    finally:
        watch.close()
        # This workflow blanks NVS to start from factory. If it did not get as
        # far as configuring the device again, it owes the rest of the suite a
        # device that is back in service — otherwise every later test faces a
        # board sitting in its portal, reporting no meter cycles, and fails
        # describing a rig it was handed.
        if not ensure_provisioned(wb, dut):
            print("  WARNING: the device is not back in service — later results "
                  "in this run are about the rig, not the firmware")
        path = _write_evidence(evidence)
        print(f"\n  evidence: {path}")
        try:
            wb.test_end()
        except Exception:
            pass
