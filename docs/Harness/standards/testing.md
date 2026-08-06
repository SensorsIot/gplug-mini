# Standard — Testing

Testing is part of the build contract, not an afterthought. Every behaviour rule
in the FSD is verifiable, and every change ships the test that proves it.

This document is the whole of testing in the Harness: the rules, and the
procedures for running a session. It is one file on purpose — the rules and the
procedures kept restating each other when they were two.

| | |
|---|---|
| What must be proved | the FSD: requirements, acceptance scenarios, state transitions, product goals and accepted risks |
| Test design and current evidence | [`testing/test-plan.yaml`](../../../testing/test-plan.yaml): capabilities, reusable actions, hierarchical test cases, open test-design decisions, release gates and results |
| How to derive, write, validate and run the plan | this document |

`testing/test-plan.yaml` is one file on purpose. It contains several different
responsibilities, but keeping them in one machine-readable document makes
traceability and release reporting deterministic:

```text
metadata
capabilities
actions
test_design_decisions
test_cases
release_gates
```

The sections are distinct even though the file is not split. A capability is not
a test, an action is not a workflow, and a workflow result is not inferred from
its atomic children.

## Tiers

Use the cheapest tier that can catch the failure. Reserve the expensive ones for
behaviour no cheaper tier can reach.

| Tier | Runs on | Speed | Catches |
|------|---------|-------|---------|
| **host** | Dev machine, plain compiler | ms, every push | Decode, OBIS mapping, scaling, cycle-boundary arithmetic, supervisor transitions |
| **target** | The ESP32-C3 alone | seconds | UART configuration, NVS, GPIO, boot behaviour |
| **bench** | Device on the Embedded Workbench with AP, broker and OTA relay | minutes | Provisioning, sessions, discovery, OTA, rollback, watchdog, resilience |
| **field** | Installed at the meter | once | Physical layer, real frames, meter power, WiFi coverage |
| **other** | CI or review | — | Artefact properties not observable at runtime |

**Bench and field differ by control, not by realism.** On the bench the peers are
ours and faults can be injected — stop the broker, drop the access point, serve an
untrusted certificate. In the field the peers are real and can only be observed.
So every fault-injection case belongs on the bench, and the field answers only
what nothing else can.

**The field tier is deliberately small because an M-Bus simulator exists**
(`decisions.md` T7). Decode and protocol cases sit at bench; field keeps only
what the laboratory cannot prove: the real meter's values, meter power and
cabinet WiFi coverage.

Field evidence is mandatory for the first product release and after a change
that can affect the meter interface, hardware, power, radio or timing. A later
firmware release may carry valid field evidence forward only when a documented
impact analysis shows that none of those areas changed. Field cases are
expensive and rarely repeatable, so anything a cheaper tier can prove must leave
the field tier.

**Four tiers are execution environments: host, target, bench, field.** `other` is
the fifth value the `tier` field accepts and the only one that is not an
environment: it is the bucket for properties no environment can observe, checked
by CI or review. A case lands there only when no device runs at all. Validation
accepts all five; prose that calls `other` a tier is being loose.

**The verification boundary is the MQTT broker** (FSD §22.0). Everything the
device does is a message, and every message can be read with a subscription — so
discovery payloads, state publications, availability and the last will are all
observable without a consumer in the loop. Home Assistant is not part of the
system under test, and there is no capability for it here. Testing through it
would prove the device, the MQTT integration and the recipe registry at once, so
a failure could not be attributed; and because discovery is retained, every bench
run would leave real devices in a live instance until someone cleared each config
topic by hand. What this does not prove is an accepted risk in FSD §4.1, not a
gap to be quietly closed by adding an instance later.

Layer mapping: **L0** is exercised transitively and has no host tests of its own —
empty cells there are expected, not gaps. **L1** interfaces split, with pure cores
at host and wire behaviour at target or bench. **L2** is host throughout.

## Test model and build order

Every test case has two independent classifications.

| Field | Values | Meaning |
|---|---|---|
| `kind` | `atomic`, `workflow` | Structural level of the test case |
| `scenario` | `standard`, `deviation`, `negative`, `security` | Nature of the situation exercised |

`kind` must not contain `standard`, `deviation`, `negative` or `security`.
Those values describe the scenario, not whether the case is atomic or
end-to-end.

### Atomic test cases

An atomic case verifies one narrow contract, boundary or prohibited outcome. It
is independently executable and normally references one or more requirement
IDs through `verifies`.

```yaml
- id: TS-034
  kind: atomic
  scenario: negative
  verifies: [FR-SUP-05]
  tier: bench
  needs: [board, wifi-ap, wifi-ap-outage]
  does: Deny WiFi association repeatedly and record attempt timestamps
  expected: Retry intervals increase, remain capped, and retries continue
  procedure: P-NETWORK
  status: not done
  reason: test not implemented
  commit:
  when:
```

Atomic cases answer:

> Which specified behaviour failed?

They are the primary source of requirement coverage and defect localisation.

### Workflow test cases

A workflow is a continuous end-to-end execution that proves a useful product,
operational or release outcome across several components. It normally references
an acceptance scenario through `validates`, or an explicitly named product goal,
operational procedure or release seam.

```yaml
- id: WF-003
  kind: workflow
  scenario: negative
  role: release-gate
  name: Recover from a 10-minute WiFi outage
  validates: [AC-3]
  children: [TS-032, TS-033, TS-034, TS-037, TS-040, TS-042, TS-110]
  tier: bench
  needs: [board, wifi-ap, wifi-ap-outage, mqtt-broker, mbus-sim]
  start_state:
    device: OPERATIONAL
    mqtt: connected
    fresh_publication: confirmed
  steps:
    - id: remove-access-point
      action: stop_access_point
    - id: observe-outage
      action: observe_for_duration
      with:
        duration_s: 600
      checkpoint_tests: [TS-033, TS-034, TS-037, TS-042]
    - id: restore-access-point
      action: start_access_point
    - id: prove-full-recovery
      action: observe_fresh_publication
      with:
        topic: gplug/<mac>/state
        minimum_messages: 2
        timeout_s: 60
      checkpoint_tests: [TS-040, TS-110]
  expected: The complete publishing service recovers without intervention
  must_not:
    - Provisioning starts
    - manual reset is required
    - a retained pre-outage message is accepted as recovery
  evidence_required:
    - AP outage timestamps
    - supervisor-state history
    - MQTT reconnection
    - fresh post-recovery publications
  status: not done
  reason: workflow not implemented
  commit:
  when:
```

Workflow cases answer:

> Does the product perform its useful job?

A workflow must cross at least one meaningful component boundary and end in an
externally useful result. Merely reaching an internal state is not enough.

### Hierarchy through `children`

`children` gives the test-design hierarchy:

```text
AC-1
└── WF-001 First commissioning
    ├── TS-030 Provisioning after an empty NVS
    ├── TS-053 WPA2 SoftAP
    ├── TS-055 captive portal
    ├── TS-057 configuration persistence
    ├── TS-031 configured WiFi
    ├── TS-045 discovery
    ├── TS-108 measurement mapping
    └── TS-110 live publication
```

The relationship has strict semantics:

- every child is an existing `kind: atomic` case;
- a child may belong to more than one workflow;
- children express traceability and checkpoint contracts;
- a workflow does **not** invoke its children as separate isolated sessions;
- the workflow executes its own ordered `steps` using reusable actions;
- workflow status is independent of child status.

Therefore both of these results are valid:

```text
all children successful + workflow failed
```

This reveals an integration or handoff defect.

```text
workflow successful + some children not done
```

This proves the journey but leaves a boundary, diagnostic or prohibited outcome
unverified.

### Reusable actions and observers

Repeated operations belong under the top-level `actions` section. Atomic cases
and workflows use the same action contracts.

```yaml
actions:

  wait_for_wifi_association:
    purpose: Confirm association with the intended AP, not merely an SSID match
    needs: [board, wifi-ap]
    inputs: [expected_ssid, expected_bssid, timeout_s]
    observes:
      - associated SSID
      - associated BSSID
      - assigned IP address
      - association timestamp
    confirms:
      - SSID matches
      - BSSID matches
      - an IP address is assigned
```

An action defines how the harness operates or observes the system. It is not a
test case and carries no pass status of its own. Typical actions include:

```text
confirm_bench_ready
erase_nvs
boot_device
wait_for_supervisor_state
wait_for_device_softap
connect_to_device_softap
request_configuration_page
submit_configuration
start_access_point
stop_access_point
wait_for_wifi_association
start_broker
stop_broker
wait_for_mqtt_session
feed_meter_cycles
silence_meter
observe_fresh_publication
observe_no_publication
read_discovery_messages
observe_discovery_contract
publish_ota_command
inspect_ota_state
flash_firmware_variant
hold_device_button
stall_watchdog_task
compare_with_real_meter
observe_for_duration
restore_bench
```

Do not create an action for a one-off sentence merely to make the YAML look
uniform. Extract one when the operation or observer is reused, requires its own
precondition checks, or has a failure mode that must not be reimplemented
differently in several cases.

### Build order

Build the suite in this order:

1. **Bring-up atomic cases** where hardware facts are still unproven.
2. **Standard atomic cases** for the ordinary component behaviour.
3. **Bread-and-butter workflows** proving the useful product journey.
4. **Deviation cases and workflows** for normal alternatives.
5. **Negative cases and recovery workflows** for justified faults.
6. **Security cases** derived from the threat profile.
7. **Field workflows** only for claims no cheaper tier can establish.

Bring-up that only interrogates the rig is not a test case. Record the
measurement under the capability and give it no test ID. Bring-up that discharges
an FSD requirement remains an atomic `scenario: standard` case.

All host tests run in full before a target or bench session. They cost
milliseconds, so selection only creates blind spots.

Keep the balance tilted toward `standard` and `deviation`. A suite dominated by
fault injection can accumulate impressive numbers without proving that the
product performs its basic job.

`tools/report.py` must report counts separately by `kind`, `scenario`, `tier`,
status and release role.

---

# Creating `testing/test-plan.yaml`

This section is normative. A person or AI generating or revising the plan must
follow it in order.

## Source authority

Use these sources, in descending order of authority:

1. the current approved or draft FSD;
2. existing stable test IDs and test definitions in `test-plan.yaml`;
3. settled design decisions and interface specifications referenced by the FSD;
4. measured workbench facts recorded as capability notes;
5. this testing standard.

Do not replace an FSD contract with a convenient test. When a contract cannot be
executed with the current rig, preserve it and expose the missing capability or
test-design decision.

Do not invent execution results. Plan generation may create or revise
definitions, but `status`, `reason`, `commit` and `when` belong to the runner.
When migrating an existing plan, preserve those fields unchanged unless the
existing record is explicitly retired.

## Required top-level structure

Generate one YAML document in this order:

```yaml
metadata:
  ...

capabilities:
  ...

actions:
  ...

test_design_decisions:
  ...

test_cases:
  ...

release_gates:
  ...
```

Ordering is for reviewability; references, not position, define relationships.

## `metadata`

At minimum:

```yaml
metadata:
  project: gPlug-mini
  document: Hierarchical test plan
  source: testing/test-plan.yaml
  schema_version: "1.1"

  design:
    single_file: true
    test_case_kinds: [atomic, workflow]
    scenarios: [standard, deviation, negative, security]
    statuses: [not done, successful, failed, blocked, retired]
    hierarchy: >
      Workflows list atomic children for traceability and execute continuous
      steps using actions.
    result_rule: >
      Workflow status is independent of child status.
```

A schema migration also carries a `migration:` block recording what moved, and
that block is deleted once the migration is done — a one-time event kept in
`metadata` reads, a year later, as a permanent property of the schema.

When converting a flat schema to this one:

1. preserve every existing test ID;
2. move its old `kind` value to `scenario`;
3. set `kind: atomic`;
4. preserve `verifies`, `tier`, `needs`, `does`, `expected`, `procedure`,
   `status`, `reason`, `commit` and `when`;
5. convert an explicitly withdrawn case to `status: retired`;
6. do not silently fix or invalidate prior evidence during plan generation.

Evidence quality is reviewed separately from plan structure.

## `capabilities`

A capability says what the environment can provide. It must distinguish
machine-readable parameters from human notes.

```yaml
capabilities:

  wifi-ap:
    what: Workbench access point
    available: true
    parameters:
      ssid: wb-7cb1c2
      bssid: d8:3a:dd:7c:b1:c2
      passphrase: benchtest123
      channel: 6
      subnet: 192.168.168.0/24
      gateway: 192.168.168.1
    notes: ...
    limitation: ...

  mqtt-broker:
    what: Bench MQTT broker
    available: true
    parameters:
      scheme: mqtt
      host: 192.168.0.168
      port: 1883
      anonymous: true
      uri: mqtt://192.168.0.168:1883
```

Rules:

- `available` describes the current environment, not whether a requirement is
  important;
- values referenced by actions or workflows must be structured under
  `parameters` or `fixtures`, not hidden only inside prose;
- measurements of rig behaviour belong in `notes` or `limitation`;
- an unavailable capability remains in the plan when a test needs it;
- never substitute reset for power loss, a simulator for a real meter, or a
  firmware seam for the one required physical-path test.
- represent the manually operated actual button as `physical-button`; keep
  `button-gpio` only for automated electrical actuation when such hardware exists.

For gPlug-mini, define at least:

```text
board
mbus-sim
mqtt-broker
wifi-ap
wifi-ap-outage
ota-relay
physical-button
button-gpio
power-cycle
power-cycle-endurance
second-board
real-meter
```

Two of those pairs exist because one name was covering two capabilities, and in
both cases the merged name reported work as blocked that a person can do today:

- `physical-button` is the real button, pressed by a human; `button-gpio` is
  electrical actuation from the workbench, which this board cannot receive at all
  (SLOT1 is native USB, and the Pi header reaches nothing).
- `power-cycle` is **one** supply interruption, confirmed through
  `POST /api/human-interaction`; `power-cycle-endurance` is repeated unclean
  power loss under program control, which needs switched VBUS and does not exist.

The rule underneath both: a capability is defined by what the test needs, not by
how the equipment is wired. One operator-driven power cycle and a hundred
automated ones are different capabilities that happen to look alike.

The OTA relay must contain fixture references for:

```text
valid image
image unable to reach the broker
self-signed certificate endpoint
expired certificate endpoint
```

Unknown fixture URLs remain `null` with a note explaining what must be created.

## `actions`

Each action is keyed by a stable verb phrase and may contain:

```yaml
action_name:
  purpose: ...
  needs: [...]
  inputs: [...]
  observes: [...]
  confirms: [...]
```

Use `observes` for collected facts and `confirms` for the action's own successful
completion. An action must not claim the product requirement passed; that is the
test case's oracle.

Every action used by a workflow step must exist. Every action's `needs` must
refer to declared capabilities.

## Atomic case schema

Required fields:

```yaml
- id: TS-###
  kind: atomic
  scenario: standard | deviation | negative | security
  verifies: [FSD-ID, ...]
  tier: host | target | bench | field | other
  needs: [...]
  does: ...
  expected: ...
  procedure: P-TARGET | P-METER | P-NETWORK | P-OTA | P-FIELD | ...
  status: not done | successful | failed | blocked | retired
  reason:
  commit:
  when:
```

Rules:

- `verifies` contains stable FSD requirement IDs only;
- `does` describes the actual stimulus, not the intended implementation;
- `expected` states the complete observable oracle for this case;
- when the FSD has a Must NOT contract, the case must observe it explicitly;
- one requirement may require several atomic cases;
- one atomic case may verify several requirements only when one stimulus and one
  coherent oracle genuinely establish them together;
- use the cheapest tier capable of observing the failure;
- `other` is permitted only for build or review properties where no device runs;
- a test must be capable of failing;
- absence requires a complete observation window and a positive completion
  marker.

## Workflow case schema

Every workflow must contain:

```yaml
- id: WF-###
  kind: workflow
  scenario: standard | deviation | negative | security
  role: release-gate | field-acceptance | regression | diagnostic
  name: ...
  validates: [AC-#, product-goal:..., operational-procedure:..., release-seam:...]
  children: [TS-..., ...]
  tier: bench | field
  needs: [...]
  start_state:
    ...
  steps:
    - id: ...
      action: ...
      with:
        ...
      expect:
        ...
      checkpoint_tests: [TS-..., ...]
  expected: ...
  must_not:
    - ...
  evidence_required:
    - ...
  status: ...
  reason:
  commit:
  when:
```

Rules:

- `children` contains only existing atomic IDs;
- each step uses a declared action;
- `checkpoint_tests` must be a subset of the workflow's children;
- the start state must be positively establishable;
- every important handoff has a checkpoint;
- the final `expected` is externally useful, not merely an internal state;
- `must_not` includes the failure modes that could produce a false pass;
- `evidence_required` names the evidence needed later without claiming it exists;
- a timeout is explicit, or references a test-design decision;
- free text such as `enough cycles`, `until done` or `reasonable tolerance` is
  forbidden in an executable field.

## Required gPlug-mini workflow catalogue

Generate these workflows unless the FSD explicitly supersedes the underlying
claim.

| ID | Name | Primary source | Tier | Role |
|---|---|---|---|---|
| `WF-001` | Commission a factory-new device | `AC-1` | bench | release-gate |
| `WF-002` | Accumulate energy across a simulated day | `AC-2` | bench | release-gate |
| `WF-003` | Recover from a 10-minute WiFi outage | `AC-3` | bench | release-gate |
| `WF-004` | Deliver and confirm a firmware update remotely | `AC-4` | bench | release-gate |
| `WF-005` | Roll back an image that cannot reach the broker | `AC-5` | bench | release-gate |
| `WF-006` | Validate the installed real meter | `AC-6` | field | field-acceptance |
| `WF-007` | Operate for 24 hours on meter power with cabinet WiFi | `AC-7` | field | field-acceptance |
| `WF-008` | Recover from a broker outage | product goal: survive broker outage | bench | release-gate |
| `WF-009` | Continue operation through meter silence | product goal: survive meter outage | bench | release-gate |
| `WF-010` | Smoke-test the production firmware image | production/simulation build seam | bench | release-gate |
| `WF-011` | Reconfigure an installed device | FSD operational procedure | bench | release-gate |
| `WF-012` | Recover useful operation after a watchdog reset | unattended recovery goal | bench | release-gate |
| `WF-013` | Resume normal operation after power restoration | normal unattended startup goal | bench | release-gate |

The first seven execute the FSD acceptance scenarios. The additional six close
the ordinary startup, product-goal and release seams that isolated requirements
do not prove as continuous journeys.

For each workflow:

1. find the atomic cases whose contracts are exercised;
2. list them as `children`;
3. create continuous steps from reusable actions;
4. attach relevant children to steps through `checkpoint_tests`;
5. define an externally useful final oracle;
6. add prohibited false-pass outcomes;
7. list the evidence required to support the final claim.

## Test-design decisions

When the FSD does not provide an exact executable limit, do not guess. Add a
top-level decision:

```yaml
test_design_decisions:

  - id: TDD-001
    status: open
    subject: Definition of a simulated day
    needed_by: [WF-002]
    decision_required:
      - number of cycles
      - time model
      - initial and final counters
      - permitted publication loss
      - accumulation oracle
    constraint: >
      Must prove scaling, integer precision, total_increasing behaviour and delta.
```

A workflow references it as:

```yaml
with:
  test_design: decision:TDD-001
```

**The decisions themselves live in the plan, never here.** They are product test
parameters — cycle counts, timeouts, tolerances — and a number written in two
places drifts in one of them. This document owns the rule that unknown limits
become a `TDD-*` rather than a guess; `test_design_decisions` in
`testing/test-plan.yaml` owns the values, each with its rationale.

Change one only through an explicit revision that records the reason in the
decision's own `rationale`, so the next reader sees a decision rather than a
number that was always there.

## Release gates

Release gates are part of test design and live in the plan even before any test
runs.

```yaml
release_gates:

  firmware_release:
    status: proposed
    requires:
      atomic_policy:
        - every applicable Must and Should has atomic coverage
        - every release-applicable atomic case has a current successful result
        - no failed or stale release-applicable case remains
      workflows:
        - WF-001
        - WF-002
        - WF-003
        - WF-004
        - WF-005
        - WF-008
        - WF-009
        - WF-010
        - WF-011
        - WF-012
        - WF-013

  first_product_release:
    status: proposed
    inherits: firmware_release
    additionally_requires:
      workflows: [WF-006, WF-007]
      field_decisions: [TDD-003]

  subsequent_firmware_release:
    status: proposed
    inherits: firmware_release
    field_evidence_policy: >
      WF-006 and WF-007 may be carried forward only after an impact analysis
      confirms that meter handling, hardware, power, WiFi and timing are unchanged.
```

Tagging remains the human assertion that the bench gate passed, but it must be
based on the generated gate report rather than memory or a raw test count.

## Plan-generation algorithm

Generate or revise the plan with this procedure:

1. **Read the FSD completely.**
   Extract every requirement ID, priority, stimulus, expected result, Must NOT
   outcome, acceptance scenario, state transition, product goal, degraded mode,
   operational procedure and release rule.
2. **Load the existing plan.**
   Preserve stable IDs and runner-owned result fields.
3. **Migrate the classification.**
   Move old `kind` to `scenario`; set existing cases to `kind: atomic`.
4. **Compute atomic coverage.**
   Every Must and Should requirement must have one or more atomic cases. Report
   uncovered contracts; do not hide them.
5. **Normalise capabilities.**
   Separate structured parameters and fixtures from notes and limitations.
6. **Extract reusable actions.**
   Derive them from repeated procedure steps and workflow needs.
7. **Create acceptance workflows.**
   One primary workflow for each `AC-*`.
8. **Create product and release workflows.**
   Add broker recovery, meter-silence recovery, production-image smoke,
   reconfiguration, watchdog recovery and normal startup after power restoration.
9. **Build the hierarchy.**
   Assign atomic `children`, then map them to workflow checkpoints.
10. **Resolve executable values.**
    Use FSD constants where available. Create `TDD-*` entries where they are not.
11. **Define evidence contracts.**
    Every workflow lists the evidence later required to support its result.
12. **Define release gates.**
    Separate firmware release, first product release and subsequent release.
13. **Validate the complete YAML.**
    Fail generation on broken references or forbidden placeholders.
14. **Write one deterministic YAML document.**
    Do not modify source documents or execution evidence as a side effect.

## Static validation rules

The plan generator must reject a plan when any of these is true:

- duplicate test case IDs;
- unknown `kind`, `scenario`, `tier`, `role` or `status`;
- a Must or Should requirement has no atomic test;
- a workflow validates no acceptance scenario, goal, procedure or release seam;
- a workflow child does not exist or is not atomic;
- a checkpoint test is not a workflow child;
- a workflow action is undeclared;
- an action or test needs an undeclared capability;
- a capability reference points to a missing structured parameter or fixture;
- a `decision:TDD-*` reference has no decision definition;
- a release gate references an unknown workflow;
- an executable field contains vague placeholders such as `enough`,
  `reasonable`, `as needed` or `until done`;
- a workflow has no `must_not` or `evidence_required`;
- an existing runner-owned result was changed without an explicit migration
  record.

Warnings, rather than hard failures, are appropriate when:

- a workflow has no `role: release-gate`;
- a scenario balance is dominated by negatives;
- a child belongs to no workflow;
- an acceptance workflow has only one component or no interface handoff;
- a field case duplicates a claim already fully observable on the bench;
- an open TDD blocks a release-gate workflow.

## Generated reports

From the plan, generate at least:

```text
atomic coverage by requirement
workflow coverage by acceptance scenario
product-goal and operational-procedure coverage
state-transition coverage
test counts by kind, scenario, tier and status
capability blockers
open TDD blockers
release-gate readiness
stale-result list
orphan atomic tests
broken or missing hierarchy references
```

Do not report only `passed / total`. Atomic tests and workflows prove different
things and must remain separate in every summary.

---

## Rules

- **Every Must/Should rule has a test case.** A rule with no case is an untested
  contract — surface it as a gap rather than leaving it silent.
- **Every change ships its test.** Bug fixes write the regression test **first**,
  and it must fail before the fix.
- **Test cases name the requirement IDs** they verify, so coverage is computed
  rather than asserted.
- **Coverage is generated, never hand-maintained.** A hand-filled "Covered / GAP"
  column is stale the moment a test changes.
- **The supervisor is covered per transition, not per state.** The state model has
  32 rows; reaching `OPERATIONAL` proves nothing about the four edges into it.
- **A test must be able to fail.** One that asserts a payload has the right keys
  passes with the meter disconnected. Prefer one test that proves the thing works
  over several that assert shape.
- **Never tune a threshold to make a test pass.** A failing test is evidence until
  proven otherwise; find the defect first.
- **A bench pass is a few proper signals decoded, not every one.** The M-Bus
  simulator is not a meter and does not pretend to be: it drops bytes, and
  chasing that is not this project's work. Assert across several cycles and
  require one good one. What the rig loses is recorded against the *capability*,
  never against the firmware — a rig characteristic filed as a defect sends the
  next person hunting code that is doing nothing wrong.
- **Assert the prohibited outcome, not just the happy one.** The FSD's verification
  contracts carry a *Must NOT happen* column for a reason: a rollback test that
  only checks "did it recover" passes when the device recovers by rebooting, which
  is the failure being tested for.
- **Acceptance scenarios become executable workflows.** A label or a list of
  atomic tests is not an acceptance test.
- **Workflows prove handoffs.** Persisted configuration must become the WiFi
  connection, the MQTT session, discovery and live measurements in one continuous
  execution.
- **Do not derive workflow status from children.** Isolated component passes do
  not establish integration.
- **Every workflow has evidence requirements.** They specify what a later run
  must capture; they never claim the evidence already exists.
- **Unknown values become TDDs.** Do not write vague or invented limits into an
  executable workflow.
- **Definitions and results have different owners.** A generator writes test
  design; the runner writes `status`, `reason`, `commit` and `when`.

## Test framework

**Host tier: CMake and CTest, C++20, under `testing/host/`.** The same language and
standard as the firmware, so a host test exercises the code that ships rather
than a reimplementation of it — a suite in another language can only test a
second version of the logic, which is the one kind of test that cannot fail
usefully. No test framework is pulled in: assertions and one CTest entry per case
are enough, and each case names the property that breaks rather than reporting
"the decode test failed".

`dlms_parser` is fetched by tag, matching the version the firmware pins, because
these tests exist to notice when its behaviour changes. A branch would let it
move underneath them.

```bash
cmake -S testing/host -B build/host && cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

**Target tier: Unity**, which ESP-IDF bundles.

## The release gate

Bench tests need the Embedded Workbench, which a GitHub-hosted runner cannot
reach. **Tagging is the human assertion that the applicable plan-defined release
gate passed** (FSD §20.2).

This is deliberate rather than a missing gate. The obligation is stronger than
"run the bench suite": generate the release-gate report from
`test-plan.yaml`, resolve every referenced atomic and workflow case against the
current baseline, and review the blockers before tagging.

The production-image smoke workflow is mandatory. After flashing the production
build on the bench, prove that it boots, joins the intended WiFi and MQTT broker,
reports the correct version, and publishes no simulated meter data without meter
input. This is the only workflow covering the seam between simulation and the
artifact that ships.

First product release additionally requires the real-meter and 24-hour field
workflows. Later firmware releases may carry those field results forward only
under the impact policy declared in `release_gates`.

## Test data

The host tier runs against published E450 captures, committed as fixtures.

A capture taken from the real meter carries the meter serial, which identifies an
address and an account, and a power trace, which shows occupancy. Substitute the
serial consistently before committing one.

## Security testing

The threat profile is in FSD §18.1 and is deliberately modest: a device in a
private basement, on a home network. Cases derive from that profile rather than
being imported wholesale.

- A security criterion must be able to **fail**. "Credentials are protected" cannot;
  "the literal passphrase does not appear in any portal response body" can.
- Where the profile does not justify a protection, it is recorded as an accepted
  risk in FSD §4 rather than written as a requirement nobody implements. Plaintext
  NVS is the worked example: `NFR-SEC-01` states the non-claim explicitly.

---

# Running a test

### Driving a device

Flashing and bench testing go through the Embedded Workbench's HTTP API. **Do not
SSH into the workbench Pi to operate it** — every operation has an endpoint, and
reaching for SSH means the API is missing a capability that should be added there.

| Purpose | Endpoint |
|---|---|
| Flash over USB | `POST /api/flash` with `bin@<offset>` parts |
| Serve an image for the device to fetch | `POST /api/firmware/upload`, then `GET /firmware/<project>/<file>` |
| MQTT broker for bench tests | `POST /api/mqtt/start` |
| WiFi AP with captive-portal provisioning | the workbench WiFi endpoints |
| Serial console | the RFC2217 proxy |
| A physical action nothing can automate | `POST /api/human-interaction` — blocks until an operator confirms |

**`POST /api/ota` is not on that list on purpose.** It runs `espota.py` against a
board speaking ArduinoOTA, which is not the mechanism FR-OTA specifies: this
device fetches a URL it is told about over MQTT. Using the endpoint would exercise
a path the product does not have.

Bench tests are Python and drive these endpoints; host tests are C++ and touch
nothing outside the process.

## Rules that hold for every run

- **Capture evidence before resetting anything.** A reset destroys the reason for
  the reset. Read the serial log, the reset reason and the rig's own status first;
  recover second.
- **Drain the serial buffer before starting.** The workbench monitor returns
  buffered output and lines from different times arrive spliced — one captured
  here turned out to be two messages joined, timestamps forty minutes apart, one
  from before a reboot. A test that matches a phrase without draining can match
  something a previous test said.
- **A timeout is a failure, not an absent result.** Nothing on the device returns
  an exit code; a board that crashes or hangs prints nothing at all, so "no FAIL
  seen" is not a pass. Require a positive completion marker.
- **Verify the precondition, and if it fails record `not done`, never `failed`.**
  A failure claims you learned something about the requirement; with a broken
  baseline you learned nothing, and the next person hunts the wrong defect.
- **Leave the rig as you found it, in every field — not only the one you
  changed.** A restore that resets the fault but not the serial length leaves the
  next test a longer telegram than it was written for, and that test fails for a
  reason that has nothing to do with the device. Assert the restored state rather
  than issuing the commands and assuming.
- **An action a person must perform is still automatable — as a prompt.**
  `POST /api/human-interaction` blocks the test until an operator confirms, which
  is how the physical button and a single power cycle are driven. Give it a
  timeout shorter than the client's own, or the client gives up first and leaves a
  modal on screen with nothing waiting for it. What this cannot do is repetition:
  a case asking for a hundred of anything needs the automated capability, and
  substituting a person there produces a test nobody ever runs twice.

## Run order

Ordered by **what a test proves**, not by what it costs. A failure early makes
everything after it unreadable, and that matters more than minutes.

| Phase | Marker | `scenario` it runs | Proves | Example |
|---|---|---|---|---|
| **0 host** | — | all | The logic, offline | Decode, OBIS mapping, cycle arithmetic |
| **1 provisioning** | `provisioning` | `standard` | A blank device can be reached at all | Portal → configured → joined |
| **2 bread-and-butter** | `breadandbutter` | `standard` | The device does its job | A measurement reaches the broker |
| **3 deviation** | `deviation` | `deviation` | Normal operation, not the simplest case | Either serial length; waking mid-transmission |
| **4 exception** | `exception` | `negative`, `security` | Faults and malfunctions | Corrupted frame, silent line, outage, rollback |

**The phase is the run order; the `scenario` is what the case is.** They are two
views of one axis and the middle column is the whole mapping: `standard` splits
across phases 1 and 2 because a device that never provisioned cannot be asked to
do its job, and `negative` and `security` share phase 4 because both inject
something. Nothing is classified twice — a plan carries `scenario`, a pytest case
carries the marker.

**Phases 1 and 2 are the gates, and their absence is a real risk.** A suite can accumulate
deviation and malfunction cases and never assert that the product works — this
one did, for a day, while a defect that stopped the device publishing entirely
was found sideways through an unrelated test that could not name the device's
topic. Write the bread-and-butter test first even though it feels too obvious to
write down.

**A deviation is not a malfunction.** Both meter serial lengths are normal
configurations; waking mid-transmission is what happens on every power-up, and
the interface spec calls resynchronisation an operating mode rather than an error
path (§4.1). Filing those under faults hides the fact that nothing tests the
ordinary case.

Cost is the tiebreaker *inside* a phase, never across them: cheap before
expensive, and anything that resets the board or injects a fault last within its
phase, so quieter tests do not pay for the recovery. Tests that prove a
negative — *no download happens* — cost their whole window every time and can
only confirm nothing, so they sort late.

Tests that prove a negative — *no download happens*, *nothing is published* —
are the worst of both: they cost their whole window every time and can only ever
confirm nothing. They go last.

Enforce this in the harness rather than by keeping the file in a tidy order.
`testing/bench/conftest.py` sorts on the phase markers above and stops the run
when a gate phase fails, since everything after it would be unreadable. Three
further markers are orthogonal to the phase and describe cost, not meaning:
`fast`, `slow` and `disruptive` — the last for anything that resets the board,
injects a fault or starts a download, which sorts late inside its phase so
quieter tests do not pay for the recovery.

An unmarked test runs in the deviation phase. That is deliberate: defaulting a new
test into a gate would give it the authority to stop the run before anyone had
decided it deserved that.

Bench addresses are not written down. gPlug-mini uses **Workbench2**; discover it
and confirm with `GET /api/info` before a session.

## P-TARGET — the board alone

**Needs** `board`.

Nothing else may be attached or running: no simulator traffic, no broker. The
point of this tier is that the chip is answering about itself.

**Before** — flash the test firmware once. All target cases live in a single
image and are selected by name over serial, so the five-minute build-and-upload
cost is paid per change, not per test.

The image is `testing/target/`, built with `idf.py -C testing/target build` and
by CI on every push. It deliberately does NOT include the firmware's `main`
component: these cases ask the chip about itself, and pulling the application in
would make a target failure name the wrong thing — as well as dragging a
third-party component's warnings into a build that has nothing to do with them.

Its output contract is two lines and both are load-bearing:

```text
RESULT <id> PASS|FAIL  <detail>
DONE <n> checks
```

`DONE` is the positive completion marker. A board that crashes mid-case prints
nothing at all, so the absence of FAIL is not a pass — the harness requires the
marker before believing any result in the run.

**Steps** — send the case name; read until the run announces it finished and how
many checks it made.

**After** — nothing to restore.

**On failure** — capture the full serial output and the reset reason before
reflashing. A crash loop that is reflashed away takes its cause with it.

---

## P-METER — the meter link

**Needs** `board`, `mbus-sim`, usually `mqtt-broker`.

**Before**
1. Confirm the simulator's reported state rather than assuming it: normally
   `mode 2`, `fault none`, `gap 0`, and the expected identity. It has been found
   on `identity none`, which makes discovery fail for a rig reason.
2. Confirm the unique workbench AP and BSSID, then start the one bench broker.
   Do not recreate the retired two-bench workaround.
3. Confirm the board is publishing, or that failure to publish is the intended
   starting condition.

**Steps** — issue the simulator directive, then watch the board. A test or
workflow states whether a directive applies to one telegram, several cycles or a
continuous interval.

**After** — restore `fault none`, `mode 2`, `gap 0`, the standard identity,
energy and scaler. Read the simulator's `status` line and assert the restored
state.

**Mode 2 is the signal to test through, mode 3 is not.** How well each one
behaves is a property of the rig that changes when the rig changes, so the
measurements live on the `mbus-sim` capability in `test-plan.yaml` and are
re-measured rather than remembered — including the ten-minute run behind the
choice of mode 2, and the reason a mode-3 case must span several cycles and
require one good one.

Read that note before reading any meter result. If what the rig is doing today
disagrees with it, the note is what gets corrected — a rig characteristic filed
as a firmware defect sends the next person hunting code that is doing nothing
wrong.

**When a result is confusing, use `mode 4`.** The counted ramp identifies how
many bytes were lost and is a diagnostic measurement of the rig unless a
specific FSD requirement makes it a test case.

---

## P-NETWORK — WiFi, portal and broker

**Needs** `board`, `wifi-ap`, `mqtt-broker`; fault injection also needs
`wifi-ap-outage`, which works — see the capability note in `test-plan.yaml`.

### The bench network — settle this before touching the board

| Thing | Value | Why |
|---|---|---|
| AP SSID | `wb-7cb1c2` | last 3 octets of the bench radio's MAC — no two benches collide |
| AP passphrase | `benchtest123` | WPA2 |
| AP subnet | `192.168.168.1/24`, DHCP `.2–.20` | third octet is the bench's own LAN host number |
| Broker | `mqtt://192.168.0.168:1883` | the bench's LAN address; the AP NATs out to it, so one address serves bench tests and device alike |
| **Never the bench's** | `192.168.4.0/24` | `192.168.4.1` is the ESP32 SoftAP default and belongs to a DUT running its portal |

Two traps, both of which answer `ok: true` while doing the wrong thing:

- `POST /api/wifi/ap_start` takes **`pass`**, not `password`. An unknown key is
  ignored and the AP comes up **open**; the board then refuses it under its
  authmode threshold and looks broken.
- **mosquitto does not survive an `rfc2217-portal` restart.** It stops silently
  and an address that worked minutes ago starts refusing connections. `POST
  /api/mqtt/start` after any service restart.

**Prove the bench before the board.** A bench fault and a firmware fault present
identically from the device side, and the device is far slower to interrogate:

```bash
ssh pi@<bench> '
  ip -4 -br addr show wlan0                                 # expect 192.168.168.1/24
  sudo grep -E "^ssid|^wpa" /tmp/wifi-tester/hostapd.conf   # expect wpa=2 + passphrase
  ip link show wlan0 | grep ether                           # note the BSSID
  timeout 8 mosquitto_sub -h 192.168.0.168 -t bench/selftest -C 1 &
  sleep 2; mosquitto_pub -h 192.168.0.168 -t bench/selftest -m bench-ok; wait'
```

It must print `bench-ok`. Nothing learned from the board counts until it does.

**An SSID is not an identity — check the BSSID.** On 2026-08-05 two benches both
answered to `gplug-bench`; the board joined the other one, took a lease on its
subnet, and that bench ran no broker at all. Three correct broker addresses were
each blamed in turn before anyone read the line the board had been printing from
the first capture:

```text
wifi:connected with <ssid>, aid = 1, channel 11, bssid = d8:3a:dd:7c:b1:c2
```

Compare that BSSID against the bench radio's own MAC before suspecting any
address. `ap_status` showing `stations: []` is not evidence of a failed join — it
read empty while the board was happily associated elsewhere. Believe the board.

**Before** — confirm the board is associated and holding a broker session, so a
later loss is attributable to the stimulus rather than to the starting state.

**Steps** — cut what the test needs cut, then watch. To interrupt MQTT for one
device without disturbing others, prefer a source-scoped firewall rule on the
broker host over stopping the broker: `DROP` produces the silent black hole that
exercises the keepalive timeout, `REJECT` produces an immediate refusal. They are
different tests.

**After** — restore the access point and the broker; confirm the board recovers
before starting the next case.

**Judge recovery by live publishing, never by a retained value.** A retained
topic read during or just after an outage returns the copy from before the
device dropped, with its counters frozen — it looks like a failure to recover
when nothing has recovered *yet*. Subscribe for longer than the publish cadence
and require at least two arrivals: the retained copy, then a fresh one.

**A last-will topic is death-only.** Nothing ever writes `LWT = online`; the will
stays `offline` from the last drop until the next. Watch the birth or status
topic for recovery, not the will.

---

## P-OTA — firmware update

**Needs** `board`, `ota-relay`, and normally `mqtt-broker`.

The workbench firmware store is the relay: upload with
`POST /api/firmware/upload` and serve the image from
`GET /firmware/<project>/<file>`. `POST /api/ota` is not the specified mechanism;
it targets ArduinoOTA and must not substitute for the device's URL-driven update.

The capability must provide structured fixtures for the valid image, the image
that cannot reach the broker, and the TLS certificate-failure cases. Missing
fixtures block only the cases that need them.

**Before** — record the running version, active slot and validation state.
Confirm a broker session, because validation depends on one.

**Steps** — publish the command carrying the image URL; observe download, write
to the inactive slot, reboot, running version, slot and whether the image is
marked valid. Use `TDD-002` for the approved maximum duration.

**After** — return the board to a known-good image and confirm it boots,
connects and publishes fresh state.

**Rollback tests must observe the prohibited outcome, not just recovery.** A
device that recovers by repeatedly booting the same broken image has not passed.
Read the reset reason, running version, active slot and validation state.

---

## P-FIELD — the installed meter

**Needs** `real-meter`; the 24-hour workflow also needs observable WiFi and MQTT
service at the installation.

Two field workflows exist:

- `WF-006` compares real meter identity and values with the meter display;
- `WF-007` proves 24 hours on meter power with cabinet WiFi and continuous fresh
  publication.

**Before** — all applicable host, target and bench gates must pass. Resolve
`TDD-003`, record the deployment SSID and BSSID, meter identity, firmware
version and comparison method.

**Steps** — first execute the real-meter comparison, then the 24-hour installed
operation workflow. Every comparison is timestamped so display rounding and
publication latency can be judged against the approved tolerance.

**After** — preserve the complete evidence package. Nothing on the installation
is changed merely to make the test repeatable.

**This is the only tier that can invalidate the bench.** The simulator emits
bytes we authored. It proves the device reads what it is given and cannot prove
that the meter, power supply and cabinet radio environment match our assumptions.
