# gPlug-mini — Project conventions

## Toolchain

ESP-IDF **6.0.2**, target `esp32c3`. No local installation is required — GitHub
Actions builds every push (`FR-BLD-07`).

Four ESP-IDF 6 behaviours are silent until a build fails:

| Behaviour | Consequence |
|---|---|
| CMake minimum is **3.22** | A `cmake_minimum_required(VERSION 3.16)` copied from a 5.x template fails to configure |
| **cJSON is not bundled** | Discovery payloads are JSON, so `espressif/cjson` is a required managed component |
| **mDNS is not bundled** | A broker entered as `homeassistant.local` cannot resolve without `espressif/mdns` — see `FR-CFG-04` |
| Managed components are solved at configure time | Adding to `idf_component.yml` does nothing until the build directory is removed or `idf.py reconfigure` runs |

Commit `dependencies.lock`. Do not commit `managed_components/` or `sdkconfig`;
`sdkconfig.defaults` is the tracked source of build configuration.

## Versioning

Versions derive from the git tag through `esp_app_desc_t` (`FR-BLD-01`), so the
string in the Home Assistant device page, the OTA log and the release asset cannot
disagree. Untagged builds append the short SHA. The simulated build appends
`-sim`.

Publishing happens only from a tag (`FR-BLD-08`).

## The release gate

Bench tests need the Embedded Workbench, which a GitHub-hosted runner cannot
reach. **Tagging is the human assertion that the bench suite passed** (FSD §20.2).

This is a deliberate position rather than a missing gate, and it carries an
obligation: run the bench suite before tagging. Automating it would require a
self-hosted runner on the bench network, which this project does not have.

After flashing the production build on the bench, confirm it boots, connects WiFi
and MQTT, and reports no meter data. That is the only check covering the seam the
simulated build cannot exercise.

## Driving a device

Flashing and bench testing go through the Embedded Workbench's HTTP API. **Do not
SSH into the workbench Pi to operate it** — every operation has an endpoint, and
reaching for SSH means the API is missing a capability that should be added there.

| Purpose | Endpoint |
|---|---|
| Flash over USB | `POST /api/flash` with `bin@<offset>` parts |
| Firmware update to a deployed board | `POST /api/ota` |
| MQTT broker for bench tests | `POST /api/mqtt/start` |
| WiFi AP with captive-portal provisioning | the workbench WiFi endpoints |
| Serial console | the RFC2217 proxy |

Bench tests are Python and drive these endpoints; host tests are C++ and touch
nothing outside the process.

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

## Naming

- Requirement IDs are `FR-<COMPONENT>-<nn>` / `NFR-<COMPONENT>-<nn>` and are
  **stable** — never renumbered. A superseded requirement is marked, not deleted,
  so historical test results stay readable.
- Test cases name the requirement IDs they verify.
- Commits cite the requirement ID they serve.
- Files and modules are lower_snake_case; the component prefix matches the
  directory in [`architecture.md`](architecture.md).

