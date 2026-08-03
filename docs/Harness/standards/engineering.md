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
  `FR-ERR-03` exists because a silent decode failure is indistinguishable from a
  quiet meter.
- **Secrets are never in code or documentation** — only their location is
  documented. Test fixtures use placeholder credentials, never real ones.
- **Commits cite the requirement ID** they serve. Never commit build output,
  `managed_components/`, `sdkconfig`, or secrets.

## Language

The application is C++ — the DLMS library is C++20 (`FR-DEC-01`). Prefer plain
structs and free functions over class hierarchies; the pure cores must be callable
from a host test with no ESP-IDF headers in scope.

C++ exceptions are disabled in ESP-IDF by default and stay that way. Errors
propagate as `esp_err_t` or an explicit result type.
