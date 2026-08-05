# Standard — Engineering conventions

- **Reuse before adding** — search for an existing module or helper before writing
  new code.
- **Smallest change that satisfies the rule** — no speculative scope, no drive-by
  refactors bundled with a fix.
- **One module per component** — never fold an interface into its only consumer.
  A component that cannot be named cannot be tested. The components are listed in
  [`../project/architecture.md`](../project/architecture.md).
- **Dependencies point one way** — lower layers never include higher ones. Where
  the need seems to invert, invert it properly: the lower module exposes a
  registration hook and `app_main` wires it.
- **Extract pure cores** — separate the decision from the I/O. Cycle-boundary
  arithmetic, OBIS-to-label mapping, discovery-payload construction and the
  supervisor's transition function are free functions with no hardware, network or
  flash dependency, so the host tier can reach them.
- **Verification before commit** — the host tests and the build both pass. See
  [`testing.md`](testing.md).
- **Errors fail loudly** — actionable messages, no silent catches, no swallowed
  return codes. An ignored error is a defect even when nothing breaks yet.
  `FR-MTR-14` exists because a silent decode failure is indistinguishable from a
  quiet meter.
- **Secrets are never in code or documentation** — only their location is
  documented. Test fixtures use placeholder credentials, never real ones.
- **Commits cite the requirement ID** they serve. Never commit build output,
  `managed_components/`, `sdkconfig`, or secrets.

## Language

The application is C++ — the DLMS library is C++20 (D-D2). Prefer plain
structs and free functions over class hierarchies; the pure cores must be callable
from a host test with no ESP-IDF headers in scope.

C++ exceptions are disabled in ESP-IDF by default and stay that way. Errors
propagate as `esp_err_t` or an explicit result type.

## Toolchain

ESP-IDF **6.0.2**, target `esp32c3`. No local installation is required — GitHub
Actions builds every push (`FR-BLD-07`).

Four ESP-IDF 6 behaviours are silent until a build fails:

| Behaviour | Consequence |
|---|---|
| CMake minimum is **3.22** | A `cmake_minimum_required(VERSION 3.16)` copied from a 5.x template fails to configure |
| **cJSON is not bundled** | Discovery payloads are JSON, so `espressif/cjson` is a required managed component |
| **mDNS is not bundled** | A broker entered as `homeassistant.local` cannot resolve without `espressif/mdns` — see `FR-PRV-08` |
| Managed components are solved at configure time | Adding to `idf_component.yml` does nothing until the build directory is removed or `idf.py reconfigure` runs |

Commit `dependencies.lock`. Do not commit `managed_components/` or `sdkconfig`;
`sdkconfig.defaults` is the tracked source of build configuration.

## Versioning

Versions derive from the git tag through `esp_app_desc_t` (`FR-BLD-01`), so the
string in the Home Assistant device page, the OTA log and the release asset cannot
disagree. Untagged builds append the short SHA. The simulated build appends
`-sim`.

Publishing happens only from a tag (`FR-BLD-08`).

## Naming

- Requirement IDs are `FR-<COMPONENT>-<nn>` / `NFR-<COMPONENT>-<nn>` and are
  **stable** — never renumbered. A superseded requirement is marked, not deleted,
  so historical test results stay readable.
- Test cases name the requirement IDs they verify.
- Commits cite the requirement ID they serve.
- Files and modules are lower_snake_case; the component prefix matches the
  directory in [`architecture.md`](../project/architecture.md).
