"""Bench-tier fixtures for gPlug-mini.

Everything here drives the Embedded Workbench over HTTP. Nothing SSHes into the
Pi: every operation has an endpoint, and reaching for SSH means the API is
missing a capability that should be added there
(docs/Harness/standards/testing.md).

    pytest testing/bench --wt-url http://192.168.0.27:8080

The bench address is not written down anywhere in the repository. gPlug-mini
uses Workbench2; discover it and confirm with GET /api/info before a session.
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
        default=os.environ.get("WORKBENCH_URL", "http://192.168.0.27:8080"),
        help="Workbench portal URL",
    )


# ── Run order ───────────────────────────────────────────────────────────────
#
# Ordered by what a test proves, not by what it costs. A failure early makes
# everything after it unreadable, and that matters more than minutes.
#
#   1  host          offline, no rig — run separately, before any of this
#   2  bread-and-butter   the device does its job: telegram in, measurement
#                         published, entity discovered. Nothing else is worth
#                         reading until this passes.
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
PHASE = {"breadandbutter": 0, "deviation": 1, "exception": 2}
COST = {"fast": 0, "slow": 1}


def pytest_configure(config):
    config.addinivalue_line("markers", "breadandbutter: the device does its job — the gate")
    config.addinivalue_line("markers", "deviation: normal operation, not the simplest case")
    config.addinivalue_line("markers", "exception: a fault or malfunction")
    config.addinivalue_line("markers", "fast: completes in well under a minute")
    config.addinivalue_line("markers", "slow: needs a long observation window")
    config.addinivalue_line("markers", "disruptive: resets the board, injects a fault, or starts a download")


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


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    marks = {m.name for m in item.iter_markers()}
    if report.when in ("setup", "call") and report.failed and "breadandbutter" in marks:
        item.session.gate_failed = item.name


def pytest_runtest_setup(item):
    failed = getattr(item.session, "gate_failed", None)
    if failed and "breadandbutter" not in {m.name for m in item.iter_markers()}:
        pytest.skip(
            f"{failed} failed — the device is not doing its job, so nothing below "
            "it can be attributed"
        )


@pytest.fixture(scope="session")
def wb(request):
    driver = WorkbenchDriver(request.config.getoption("--wt-url"))
    driver.open()
    driver.ping()
    yield driver
    driver.close()


class Dut:
    """The board, and the two rules that make reading it trustworthy."""

    def __init__(self, wb):
        self.wb = wb

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

        A timeout is a failure, not an absent result. Nothing on the device
        returns an exit code, and a board that crashes or hangs prints nothing
        at all — so "no match seen" must be a positive failure, never a pass.
        """
        result = self.wb.serial_monitor(DUT, pattern=pattern, timeout=seconds)
        if not result.get("matched"):
            tail = "\n  ".join((result.get("output") or [])[-12:])
            pytest.fail(f"no line matched {pattern!r} in {seconds}s. Last output:\n  {tail}")
        return result["line"]

    def reset(self):
        self.wb.serial_reset(DUT)

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

    def command(self, text, settle=0.6):
        self.port.reset_input_buffer()
        self.port.write((text + "\r\n").encode())
        self.port.flush()
        time.sleep(settle)
        return self.port.read(4000).decode("utf-8", "replace")

    def status(self):
        """The `status` line as a dict of its key=value fields."""
        for line in self.command("status").splitlines():
            if line.startswith("OK ") and "mode=" in line:
                return dict(
                    f.split("=", 1) for f in line[3:].split() if "=" in f
                )
        pytest.fail("simulator did not answer `status` — is another client attached?")

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

    def known_state(self):
        """mode 3, no fault, identity emitted, lead-in present.

        Asserted rather than assumed. It has been found set to `identity none`,
        which makes every discovery test fail for a reason that has nothing to
        do with the device — and the failure looks like the firmware never
        learning the meter serial, which is a real defect elsewhere.
        """
        # Every field the simulator's `status` reports, not only the ones a test
        # is about to change. TS-016 switches to `serial 16` and the next test
        # then sees a longer telegram than it was written for — which is how a
        # link-health check that passed at 96% failed on the following run for a
        # reason that had nothing to do with the board.
        for c in ("fault none", "gap 0", "identity ldn", "mode 3", "serial 8",
                  f"preamble {self.PREAMBLE} 0xFF"):
            self.command(c)
        state = self.status()
        assert state.get("fault") == "none", f"simulator still has {state.get('fault')} armed"
        assert state.get("identity") != "none", "simulator is emitting no identity"
        assert state.get("serial") == "8", f"simulator serial length is {state.get('serial')}"
        assert state.get("preamble", "").startswith(str(self.PREAMBLE)), \
            f"simulator preamble is {state.get('preamble')}"
        return state

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
def dut_mac(dut):
    """The MAC the device publishes under, read from its own log.

    Not derived from anything: the topic tree is keyed on the station MAC, and a
    test that computed it a second way would agree with itself and disagree with
    the device.
    """
    for line in dut.lines(seconds=25):
        m = re.search(r"gplug/([0-9a-f:]{17})/", line)
        if m:
            return m.group(1)
    pytest.fail(
        "the device never named its own topic in 25 s — it is not connected to "
        "the broker, so nothing published to it would arrive"
    )


@pytest.fixture
def broker(wb):
    """mosquitto, started for the test and left running.

    Start it on BOTH benches when two are powered: both broadcast `gplug-bench`
    and both gateways are 10.42.0.1, the board joins whichever radio is stronger
    and cannot tell you which. Start one and the board may land on the quiet one
    and report only a TCP error.
    """
    status = wb.mqtt_status()
    if not status.get("running"):
        wb.mqtt_start()
    return wb


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
