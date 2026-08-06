"""Bench-tier fixtures for gPlug-mini.

Everything here drives the Embedded Workbench over HTTP. Nothing SSHes into the
Pi: every operation has an endpoint, and reaching for SSH means the API is
missing a capability that should be added there
(docs/Harness/standards/testing.md).

    pytest testing/bench --wt-url http://192.168.0.168:8080

The constants below are DERIVED from whichever bench is in use — the SSID from
its radio's MAC, the AP subnet from its own LAN host number — so moving benches
means re-deriving them, never copying them. Confirm with GET /api/info first.
"""

import os
import re
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from workbench_driver import WorkbenchDriver  # noqa: E402

# The board under test and the M-Bus simulator, by slot. SLOT1 is a native USB
# JTAG device, so it is reset over USB rather than through the Pi's GPIO header
# — those pins reach nothing on this bench (test-plan.yaml, button-gpio).
DUT = "SLOT1"
SIM = "SLOT3"

# Poll length for accumulating serial output. Below about two seconds the portal
# closes the connection mid-request, which surfaces as a failure in whichever
# test is polling rather than as the transport problem it is.
POLL_SECONDS = 3


def pytest_addoption(parser):
    parser.addoption(
        "--wt-url",
        default=os.environ.get("WORKBENCH_URL", "http://192.168.0.168:8080"),
        help="Workbench portal URL",
    )


# ── Run order ───────────────────────────────────────────────────────────────
#
# Ordered by what a test proves, not by what it costs. A failure early makes
# everything after it unreadable, and that matters more than minutes.
#
# These markers are the run order. The plan's `scenario` field is what a case
# IS, and the two map as: standard splits across provisioning and
# bread-and-butter, deviation is its own phase, and negative and security share
# the exception phase. Nothing is classified twice — a plan entry carries a
# scenario, a test here carries a marker (docs/Harness/standards/testing.md).
#
#   1  host          offline, no rig — run separately, before any of this
#   2  bread-and-butter   the device does its job: telegram in, measurement
#                         published, discovery published to the broker. Nothing
#                         else is worth reading until this passes.
#   3  deviation      still normal operation, just not the simplest case — both
#                     serial lengths, waking mid-transmission, a reset. The
#                     interface spec is explicit that mid-burst resynchronisation
#                     is an operating mode rather than an error path (§4.1).
#   4  exception      faults and malfunctions — corruption, silence, noise,
#                     outages, rollback.
#
# The bread-and-butter phase is the gate. Its failure skips the rest, because a
# device that is not doing its job cannot tell you anything useful about how it
# handles a corrupted frame.
#
# Cost is the tiebreaker inside a phase, not across phases: `slow` sorts after
# `fast`, and anything that resets the board or injects a fault sorts last
# within its phase so the quieter tests do not pay for the recovery.
# Provisioning comes first because it is where the device's life starts: an
# unconfigured board reaches no broker, so every measurement test downstream
# would fail for the same upstream reason and read as a dozen defects.
PHASE = {"provisioning": 0, "breadandbutter": 1, "deviation": 2, "exception": 3}
COST = {"fast": 0, "slow": 1}

# Both early phases are gates. A gate failure skips everything in a *later*
# phase — not the whole run — so a broken portal still lets its own siblings
# report, and the output says which step of the journey broke rather than
# burying it under everything it made impossible.
GATES = ("provisioning", "breadandbutter")


def pytest_configure(config):
    config.addinivalue_line("markers", "provisioning: the portal, from blank device to joined — the first gate")
    config.addinivalue_line("markers", "breadandbutter: the device does its job — the gate")
    config.addinivalue_line("markers", "deviation: normal operation, not the simplest case")
    config.addinivalue_line("markers", "exception: a fault or malfunction")
    config.addinivalue_line("markers", "fast: completes in well under a minute")
    config.addinivalue_line("markers", "slow: needs a long observation window")
    config.addinivalue_line("markers", "disruptive: resets the board, injects a fault, or starts a download")
    config.addinivalue_line("markers", "workflow: a continuous end-to-end journey, not an atomic contract")
    config.addinivalue_line("markers", "manual: needs an operator at the bench — prompts via /api/human-interaction")


def pytest_collection_modifyitems(config, items):
    def rank(item):
        marks = {m.name for m in item.iter_markers()}
        # An unmarked test sorts as a deviation: calling it bread-and-butter
        # would give it authority to skip the run, and calling it an exception
        # would bury it behind every fault case.
        phase = min((PHASE[m] for m in marks if m in PHASE), default=PHASE["deviation"])
        cost = min((COST[m] for m in marks if m in COST), default=COST["fast"])
        return (phase, cost, 1 if "disruptive" in marks else 0, item.name)

    items.sort(key=rank)


def _phase_of(item):
    marks = {m.name for m in item.iter_markers()}
    return min((PHASE[m] for m in marks if m in PHASE), default=PHASE["deviation"])


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    marks = {m.name for m in item.iter_markers()}
    # A workflow never closes a gate. Its status is independent of its children
    # by design: a failed journey with every atomic case passing is an
    # integration defect, and suppressing those atomic cases would hide the very
    # evidence that distinguishes the two. Run 4 lost eighteen tests this way —
    # WF-001 failed on a log line it was too late to see, and the meter and
    # broker suites never ran, on a device that was demonstrably operational.
    gate = next((m for m in GATES if m in marks), None)
    if "workflow" in marks:
        gate = None
    if report.when in ("setup", "call") and report.failed and gate:
        # Keep the earliest gate that broke: a later one failing is usually a
        # consequence, and naming the consequence sends the reader downstream of
        # the cause.
        prior = getattr(item.session, "gate_failed", None)
        if prior is None or PHASE[gate] < prior[1]:
            item.session.gate_failed = (item.name, PHASE[gate])


def pytest_runtest_setup(item):
    failed = getattr(item.session, "gate_failed", None)
    if failed and _phase_of(item) > failed[1]:
        name = failed[0]
        pytest.skip(
            f"{name} failed — an earlier step of the journey is broken, so "
            "nothing after it can be attributed"
        )


@pytest.fixture(scope="session")
def wb(request):
    """The workbench, or a skip that names the real problem.

    This bench has dropped off the network twice in one evening — every port
    refused, `No route to host` — and when it does, every test that touches it
    fails with a different transport error. None of those failures is about the
    firmware, and a run full of them reads like a broken device.

    A precondition that cannot be met is `not done`, never `failed`.
    """
    url = request.config.getoption("--wt-url")
    driver = WorkbenchDriver(url)
    try:
        driver.open()
        driver.ping()
    except Exception as e:              # noqa: BLE001 — any failure to reach it is the same result
        pytest.skip(f"the workbench at {url} is unreachable ({e}); nothing can "
                    "be learned about the device while it is gone")
    yield driver
    driver.close()


class Dut:
    """The board, and the two rules that make reading it trustworthy."""

    def __init__(self, wb):
        self.wb = wb
        self._stash = []      # lines captured while a reset was settling

    def drain(self):
        """Discard buffered serial output.

        The monitor returns whatever is buffered, and lines from different times
        arrive spliced — one capture here turned out to be two messages joined,
        timestamps forty minutes apart, one from before a reboot. A test that
        matches a phrase without draining first can match something a previous
        test said.
        """
        self.wb.serial_monitor(DUT, timeout=1)

    def lines(self, seconds=10):
        """Serial output collected over a wall-clock window, oldest first.

        Polled in short reads and accumulated rather than asked for in one long
        one. A single call returns whatever the buffer holds when it answers,
        which after a drain can be almost nothing — so a test asking for sixty
        seconds of output got one cycle and then reported the rig as silent.
        The fault was in the asking, and it presented as a bench failure.

        Duplicates are dropped: consecutive reads overlap, and a repeated line
        would otherwise be counted as a repeated cycle.
        """
        deadline = time.monotonic() + seconds
        seen, out = set(), []
        misses = 0
        while time.monotonic() < deadline:
            try:
                result = self.wb.serial_monitor(DUT, timeout=POLL_SECONDS)
            except Exception as e:
                # A dropped HTTP connection is not a result about the device.
                # Retrying keeps a transport hiccup from being recorded as a
                # firmware failure; giving up after several says the bench is
                # unreachable, which is a different problem and reads as one.
                misses += 1
                if misses > 3:
                    pytest.fail(f"the workbench stopped answering after {misses} tries: {e}")
                time.sleep(1)
                continue
            misses = 0
            for line in result.get("output") or []:
                if line not in seen:
                    seen.add(line)
                    out.append(line)
        return out

    def await_line(self, pattern, seconds=30):
        """A line matching `pattern`, or fail.

        Polled in short windows and matched here, rather than handing the
        pattern to one long monitor call. A single long call binds to the serial
        endpoint it had when it started, and on this board that endpoint does
        not survive a reset: the call then sits out its whole window against a
        device that is printing perfectly into a port nobody is reading.
        Measured 2026-08-06 — a 60 s pattern monitor returned nothing while a
        15 s one issued moments later caught three lines.

        A timeout is still a failure, not an absent result. Nothing on the
        device returns an exit code, and a board that crashes or hangs prints
        nothing at all — so "no match seen" must be a positive failure, never a
        pass.
        """
        rx = re.compile(pattern)
        deadline = time.monotonic() + seconds
        seen = list(self._stash)
        self._stash = []
        for line in seen:
            if rx.search(line):
                return line
        while time.monotonic() < deadline:
            try:
                result = self.wb.serial_monitor(DUT, timeout=POLL_SECONDS)
            except Exception:
                time.sleep(1)
                continue
            for line in result.get("output") or []:
                if line not in seen:
                    seen.append(line)
                if rx.search(line):
                    return line
        tail = "\n  ".join(seen[-12:]) or "(nothing at all)"
        pytest.fail(f"no line matched {pattern!r} in {seconds}s. Last output:\n  {tail}")

    def reset(self, settle=True):
        """Reset the board and wait until its console can be read again.

        On this bench the reset goes over JTAG — `/api/serial/reset` reports
        `method: jtag` and ignores any method asked for, because SLOT1 is a
        native-USB part where reset and console share one interface. The CPU
        restarts immediately; the USB device re-enumerates a moment later, and
        anything that reads in between reads silence.

        Not waiting here cost a whole suite run on 2026-08-06: every test that
        reset the board saw an empty console and failed as though the firmware
        were dead, while a hard reset over the same cable printed normally.
        """
        self.wb.serial_reset(DUT)
        self._stash = []
        if not settle:
            return
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                slot = self.wb.get_slot(DUT)
            except Exception:
                slot = {}
            if slot.get("devnode") and slot.get("state") in ("idle", "monitoring"):
                break
            time.sleep(1)
        # The slot reports ready before the CDC is actually carrying data. Read
        # during the settle rather than sleeping through it: the firmware repeats
        # its reset reason once the network is up, and a bare sleep here is
        # long enough to miss that line entirely — TS-084 failed on exactly that,
        # waiting 60 s for a line printed while this function was asleep.
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            try:
                self._stash += self.wb.serial_monitor(DUT, timeout=2).get("output") or []
            except Exception:
                time.sleep(1)

    def boot_banner(self, seconds=20):
        """Reset and return the identifying banner, so the running image is known."""
        self.drain()
        self.reset()
        return self.await_line(r"gPlug-mini .* build", seconds)


@pytest.fixture
def dut(wb):
    return Dut(wb)


class Sim:
    """The M-Bus simulator, driven over its console.

    The workbench HTTP API can read serial but not write it, so the console goes
    over the RFC2217 proxy — which the testing standard names as the serial
    endpoint, so this is the documented path and not a way around the API. The
    console runs at 115200; the M-Bus line it emits is the 2400 baud one the
    board reads, and the two are unrelated.
    """

    CONSOLE_BAUD = 115200

    def __init__(self, host, port, attempts=5):
        import serial  # local: only bench runs need pyserial

        # Retried, because the proxy takes a moment to release a client that has
        # just disconnected and refuses the next one meanwhile. Anyone who has
        # been driving the simulator by hand leaves exactly that window open,
        # and this fixture is session-scoped — so failing at open errors every
        # test that touches the simulator, all of them reporting a serial
        # problem rather than the one-second race that caused it.
        self._host, self._port_no = host, port
        last = None
        for attempt in range(attempts):
            try:
                self.port = serial.serial_for_url(
                    f"rfc2217://{host}:{port}", baudrate=self.CONSOLE_BAUD, timeout=2
                )
                return
            except Exception as e:   # noqa: BLE001 — any open failure is worth a retry
                last = e
                time.sleep(2)
        pytest.fail(
            f"could not open the simulator console after {attempts} tries: {last}. "
            "Another client is probably attached — the proxy takes one."
        )

    def command(self, text, settle=0.6, retries=2):
        """Send a console command, reopening the port if it has died.

        The RFC2217 link drops under a long run — measured on 2026-08-06, it
        failed 20 minutes in with `timeout while waiting for option 'purge'` and
        then `BrokenPipeError`, and because this connection is session-scoped
        every later test that touched the simulator errored in setup. Five did.
        None of those errors said anything about the device.
        """
        for attempt in range(retries + 1):
            try:
                self.port.reset_input_buffer()
                self.port.write((text + "\r\n").encode())
                self.port.flush()
                time.sleep(settle)
                return self.port.read(4000).decode("utf-8", "replace")
            except Exception as e:      # noqa: BLE001 — any link failure is worth reopening
                if attempt == retries:
                    pytest.fail(f"simulator console failed after {retries} reopen(s): {e}")
                print(f"\n  simulator console dropped ({e}) — reopening")
                self._reopen()

    def _reopen(self):
        """Reopen, or say plainly that the bench is gone.

        Returning silently with a closed port turned one dropped link into
        `PortNotOpenError` on every later call, and five tests errored quoting a
        serial exception that said nothing about why. If the proxy cannot be
        reached at all the bench is down, and that is a precondition failure —
        not a result about the firmware.
        """
        import serial
        try:
            self.port.close()
        except Exception:
            pass
        time.sleep(2)
        last = None
        for _ in range(5):
            try:
                self.port = serial.serial_for_url(
                    f"rfc2217://{self._host}:{self._port_no}",
                    baudrate=self.CONSOLE_BAUD, timeout=2)
                return
            except Exception as e:      # noqa: BLE001
                last = e
                time.sleep(3)
        pytest.skip(
            f"the simulator console at {self._host}:{self._port_no} cannot be "
            f"reopened ({last}). The bench is unreachable, so nothing here would "
            "say anything about the device."
        )

    def status(self):
        """The `status` line as a dict of its key=value fields."""
        for line in self.command("status").splitlines():
            if line.startswith("OK ") and "mode=" in line:
                return dict(
                    f.split("=", 1) for f in line[3:].split() if "=" in f
                )
        pytest.fail("simulator did not answer `status` — is another client attached?")

    # The signal the bread-and-butter tests use: one short frame carrying power
    # and energy, with the meter identity, every five seconds.
    #
    # Mode 3's full E450 telegram is 417 bytes over three GBT blocks and the rig
    # delivers about 370 of them, so the identity never survives — that is the
    # simulator, not the firmware, and it belongs in a deviation test rather than
    # under the check that the product works at all.
    NORMAL_MODE = "mode 2"

    # Bytes of lead-in before the first frame. Without it the board decodes
    # nothing at all: it loses a few bytes at the head of every burst, and those
    # bytes are the opening `7E A0 95` of frame 1. Losing frame 1 loses GBT
    # block 1, and blocks 2 and 3 arriving perfectly then reassemble into
    # nothing. Measured 2026-08-05: 0 bytes of preamble decodes 0 objects, 6
    # decodes 6, and so do 12 and 24.
    #
    # This compensates for the head loss so decoding can be tested at all. It
    # does not explain or excuse it — see TS-025, which is that question.
    PREAMBLE = 8

    # What the rig must be in before any test reads a result from the board.
    WANTED = {
        "fault": "none",
        "gap": "0",
        "silence": "0",
        "identity": "ldn",
        "serial": "8",
    }
    SETTERS = {
        "fault": "fault none",
        "gap": "gap 0",
        "silence": "silence 0",
        "identity": "identity ldn",
        "serial": "serial 8",
    }

    def set_verified(self, field, value, attempts=4):
        """Set one field and prove it took, because a bare command can vanish.

        The console interleaves the simulator's own emit log with command
        replies and a directive can be swallowed in that traffic. `known_state`
        already retries for the fields it owns; anything a test changes on its
        own needs the same treatment. WF-001 set `identity ldn` with one
        unverified call, it was lost, the device never decoded a meter serial,
        and the workflow failed reporting that discovery was never published —
        which was true, and entirely the rig's doing.
        """
        for _ in range(attempts):
            self.command(f"{field} {value}")
            time.sleep(1)
            if str(self.status().get(field, "")).startswith(str(value)):
                return True
        pytest.fail(
            f"the simulator would not accept `{field} {value}` after {attempts} "
            f"attempts — a rig fault, so nothing read from the board means anything"
        )

    def known_state(self, attempts=4):
        """Drive the simulator to a known state, verifying each field.

        Issued-and-assumed is not good enough here. The console interleaves the
        simulator's own emit log — one line every five seconds — with command
        replies, and a directive can be swallowed in that traffic. On 2026-08-06
        one lost `identity ldn` left the rig emitting no meter serial for a whole
        run: the board never learned a serial, never published, and seven tests
        failed or errored describing a device that was working perfectly.

        So each field is set, read back, and set again if it did not take. What
        cannot be driven into place after several attempts is a rig fault and
        says so, rather than being inherited by whatever runs next.
        """
        for attempt in range(attempts):
            state = self.status()
            wrong = {k: state.get(k) for k, want in self.WANTED.items()
                     if state.get(k) != want}
            mode_ok = str(state.get("mode", "")).startswith(self.NORMAL_MODE[-1])
            pre_ok = str(state.get("preamble", "")).startswith(str(self.PREAMBLE))
            if not wrong and mode_ok and pre_ok:
                return state

            if attempt:
                print(f"\n  simulator still {wrong or 'mis-set'} — retrying "
                      f"({attempt + 1}/{attempts})")
            for field in wrong:
                self.command(self.SETTERS[field])
            if not mode_ok:
                self.command(self.NORMAL_MODE)
            if not pre_ok:
                self.command(f"preamble {self.PREAMBLE} 0xFF")
            time.sleep(1)

        state = self.status()
        pytest.fail(
            "the simulator would not reach a known state after "
            f"{attempts} attempts: {state}. This is a rig fault — nothing read "
            "from the board while it persists says anything about the firmware."
        )

    def close(self):
        try:
            self.port.close()
        except Exception:
            pass


@pytest.fixture(scope="session")
def sim_console(request):
    """One console connection for the whole run.

    The RFC2217 proxy accepts a single client, so a per-test connection races
    its own teardown: the second test opens the port before the first has
    finished releasing it and the reader thread dies. That presents as a serial
    error in whichever test happens to be second, which is not where the fault
    is.
    """
    host = request.config.getoption("--wt-url").split("//")[-1].split(":")[0]
    s = Sim(host, 4003)
    yield s
    s.close()


@pytest.fixture
def sim(sim_console):
    sim_console.known_state()
    yield sim_console
    # Leave the rig as it was found. A fault directive left set silently
    # corrupts every later test, and the next person debugs the firmware.
    sim_console.known_state()


@pytest.fixture(scope="session")
def broker_host(request):
    """The broker's address, from the workbench URL rather than written down."""
    return request.config.getoption("--wt-url").split("//")[-1].split(":")[0]


@pytest.fixture
def dut_mac(dut, broker):
    """The MAC the device publishes under, read from the broker's own topics.

    Not from the device's log. The topic name appears there once, early, in a
    line that is gone by the time most tests look — three update tests errored
    in setup on 2026-08-06 with "the device never named its own topic", on a
    board that was connected and publishing. And not computed here either: a
    harness that derives the topic a second way agrees with itself and disagrees
    with the device.

    The broker is the right source. The device's own retained status topic names
    it, which is both authoritative and durable.
    """
    import paho.mqtt.client as mqtt

    seen = []
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = lambda _c, _u, m: seen.append(m.topic)
    client.connect(BENCH_HOST, 1883, 60)
    client.subscribe("gplug/+/status", qos=0)
    client.subscribe("gplug/+/state", qos=0)
    client.loop_start()
    try:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline and not seen:
            time.sleep(2)
    finally:
        client.loop_stop()
        client.disconnect()

    for topic in seen:
        parts = topic.split("/")
        if len(parts) > 1 and len(parts[1]) == 17:
            return parts[1]
    pytest.fail(
        "no gplug/<mac>/ topic reached the broker in 60 s — the device is not "
        "publishing, so nothing addressed to it would arrive either"
    )


# The bench identity, in one place because it was scattered and drifted.
#
# The SSID carries the bench radio's MAC and the AP subnet carries the bench's
# own LAN octet, so no two benches can be confused for one another. That is the
# fix for the failure of 2026-08-05: two benches both answered to `gplug-bench`,
# the board joined the other one, and three correct broker addresses were each
# blamed in turn before anyone read the BSSID in the board's own log.
BENCH_SSID = "wb-7cb1c2"           # last 3 octets of the bench wlan0 d8:3a:dd:7c:b1:c2
BENCH_PASS = "benchtest123"

# Channel 1, and this is measured rather than chosen. On channel 6 the DUT
# associated and was disassociated within seconds, over and over — reason 4
# (association expired), reason 2 (auth expired), reason 201 — with zero
# brownouts and zero resets across five minutes of uptime, so neither the supply
# nor the firmware was ending it. Moved to channel 1 with the same image, it
# joined at once and held for as long as it was watched.
#
# A scan says 2437 MHz carries a solar inverter's radio at -82 dBm and little
# else, so this is not ordinary congestion; something on or near that channel is
# hostile to this link in a way an AP list does not show. Channel 1 is busier on
# paper — several neighbours at -45 dBm — and works anyway, which is the whole
# point of measuring rather than reasoning about it.
BENCH_CHANNEL = 1
BENCH_HOST = "192.168.0.168"       # the bench on the LAN; NAT makes it reachable from the AP
BROKER_URI = f"mqtt://{BENCH_HOST}:1883"
BENCH_AP_GATEWAY = "192.168.168.1" # the bench as an AP; 192.168.4.1 belongs to the DUT's portal


@pytest.fixture
def broker(wb):
    """mosquitto, started for the test and left running.

    One bench, one broker. An earlier version of this docstring said to start a
    broker on both benches because the board "cannot tell you which" it joined —
    that was wrong twice over. The board logs the BSSID it associated with, and
    the actual problem was a shared SSID, which a second broker papers over
    instead of fixing.

    Mosquitto does not survive an rfc2217-portal restart, so this checks rather
    than assumes: a stopped broker refuses connections at an address that was
    working minutes earlier.
    """
    status = wb.mqtt_status()
    if not status.get("running"):
        wb.mqtt_start()

    # And the access point, because a broker the device cannot route to is no
    # broker at all. The provisioning fixture stops the AP so the bench radio is
    # free to scan for the device's own SoftAP, and a portal test that fails
    # before it restarts the AP leaves every later test facing a device that is
    # provisioned, correct, and unable to reach anything — reporting itself as
    # `disconnected from wb-7cb1c2, reason 201`, which reads as a firmware
    # fault. Measured on 2026-08-06: three bread-and-butter tests regressed
    # exactly this way after passing twice.
    if not (wb.ap_status() or {}).get("active"):
        wb.ap_start(BENCH_SSID, BENCH_PASS, channel=BENCH_CHANNEL, internet=True)
        # The device is in backoff, capped at 30 s (FR-SUP-05), so give it time
        # to notice the network exists again before anything reads a result.
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if (wb.ap_status() or {}).get("stations"):
                break
            time.sleep(3)
    return wb


NVS_OFFSET = "0x9000"
NVS_SIZE = 24 * 1024      # the nvs partition in partitions.csv


@pytest.fixture
def unprovisioned(wb, dut):
    """A device with no stored configuration, so it enters Provisioning.

    Without this a portal test passes or fails on whatever the previous test
    left behind: a provisioned board boots straight to operational and never
    raises the portal, so the test fails describing a portal defect that does
    not exist.

    Blanking NVS is the available lever. FR-SUP-06's button hold would be
    gentler, but it needs a GPIO wired to the board and this bench has none —
    which is recorded as the `button-gpio` capability being unavailable.
    """
    # The bench radio does one thing at a time. Every portal test either scans
    # for the device's SoftAP or joins it, and neither is possible while the
    # bench is being an access point — a scan run in AP mode returns nothing and
    # the test reports the DUT as "not on the air" when it is broadcasting
    # perfectly. Asserted here rather than trusted, because a test that failed
    # before its own cleanup leaves the AP up for whatever runs next.
    wb.ap_stop()

    result = wb.flash_region("SLOT1", "esp32c3", NVS_OFFSET, b"\xff" * NVS_SIZE)
    assert result.get("ok"), f"could not blank NVS, so the portal cannot be reached: {result}"
    yield
    # Deliberately not restored. The device is left as the test left it, and the
    # provisioning phase ends by configuring it for everything downstream.


class MqttWatch:
    """A subscription held open across steps, with arrival times kept.

    Workflows need to say *when* a message arrived relative to a stimulus, which
    a subscribe-collect-disconnect helper cannot: by the time it returns, the
    ordering it was supposed to establish is gone. So this stays open for the
    length of the journey and every message is stamped as it lands.

    `retain` is recorded per message because a retained copy and a live publish
    are indistinguishable once the flag is discarded — and reading the first for
    the second is the standard way to conclude a dead device is healthy.
    """

    def __init__(self, host, topics, port=1883):
        import paho.mqtt.client as mqtt

        self.messages = []          # (monotonic, topic, payload, retain)
        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self._client.on_message = self._on_message
        self._client.connect(host, port, 60)
        for t in topics:
            self._client.subscribe(t, qos=0)
        self._client.loop_start()

    def _on_message(self, _client, _userdata, msg):
        self.messages.append((
            time.monotonic(), msg.topic,
            msg.payload.decode("utf-8", "replace"), bool(msg.retain),
        ))

    def since(self, mark, topic_prefix=""):
        return [m for m in self.messages
                if m[0] > mark and m[1].startswith(topic_prefix)]

    def clear_retained(self, topics):
        """Publish an empty retained payload, which deletes a retained message.

        Needed before asserting that something *appears*: a config left on the
        broker by an earlier run satisfies the assertion without the device
        having done anything at all.
        """
        for t in topics:
            self._client.publish(t, payload=None, qos=1, retain=True)
        time.sleep(2)

    def mark(self):
        return time.monotonic()

    def close(self):
        try:
            self._client.loop_stop()
            self._client.disconnect()
        except Exception:
            pass


def wait_until_connected(wb, dut, seconds=120):
    """Wait until the device is demonstrably on THIS bench's network.

    Two independent proofs, either of which is sufficient, because each fails
    for reasons that have nothing to do with the device:

    * a station in this AP's list — evidence from the peer, but `ap_status` has
      been measured lagging `iw dev wlan0 station dump` by up to twenty seconds;
    * `state=operational ... broker=up` from the device — and this one is
      stronger than it looks, because the broker lives on the bench LAN and is
      reachable only by routing through the bench AP. A device that reports the
      broker up has necessarily joined this network.

    What this deliberately does NOT do is wait for `wifi:connected with ...
    bssid = ...`. That line is printed once, early, and is routinely gone before
    a test starts watching — TS-057 and WF-001 each failed on it while the board
    sat there publishing measurements. The BSSID still gets checked when the
    line is in the buffer; its absence is not evidence of anything.

    Returns (connected, how) so the caller can record which proof it got.
    """
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        stations = (wb.ap_status() or {}).get("stations") or []
        if stations:
            return True, {"stations": stations}
        for line in dut.lines(seconds=8):
            if "state=operational" in line and "broker=up" in line:
                return True, {"diag": line}
    return False, {}


def decoded_values(line):
    """The object count from a `cycle:` log line, or None."""
    m = re.search(r"cycle: \d+ bytes, (\d+) objects", line)
    return int(m.group(1)) if m else None


def a_good_cycle(dut, cycles=6, seconds=45):
    """The best cycle out of several, as a count of decoded objects.

    Never assert on one cycle. Measured with the counted ramp: two bursts in six
    arrive complete and byte-perfect, four arrive corrupted from the first byte.
    A single-cycle assertion here is flaky by construction and will be blamed on
    the firmware (test-plan.yaml, mbus-sim).
    """
    best = 0
    seen = 0
    for line in dut.lines(seconds=seconds):
        n = decoded_values(line)
        if n is None:
            continue
        seen += 1
        best = max(best, n)
        if seen >= cycles:
            break
    if seen == 0:
        pytest.fail(f"no cycle reported in {seconds}s — the meter link is silent")
    return best, seen
