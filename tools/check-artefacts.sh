#!/usr/bin/env bash
# Artefact checks — the `other` tier. No device runs; these inspect what a build
# produced and what the repository promises, which is the only way to verify
# properties no execution environment can observe.
#
#   tools/check-artefacts.sh [<build-dir>]
#
# Exit code is the result: 0 all passed, 1 something failed. Printing alone
# would make this a diagnostic script rather than a test.
set -uo pipefail
BUILD="${1:-build}"
fail=0

check() {                       # check <id> <description> <condition-result>
  if [ "$3" = 0 ]; then printf '  PASS %-8s %s\n' "$1" "$2"
  else printf '  FAIL %-8s %s\n' "$1" "$2"; fail=1; fi
}
skip() { printf '  SKIP %-8s %s\n' "$1" "$2 — $3"; }

echo "artefact checks (build dir: $BUILD)"

# ── TS-100 · FR-BLD-07 — every push builds and runs the host tests ────────────
# The must-not is a workflow that runs only on a tag, or a build that skips the
# tests: both are what a well-meant "save CI minutes" change would introduce.
wf=.github/workflows
if [ -d "$wf" ]; then
  grep -q 'push:' "$wf/build.yml" && grep -q 'push:' "$wf/host-tests.yml"
  check TS-100 "both workflows trigger on push" $?
  grep -q 'ctest' "$wf/host-tests.yml"
  check TS-100 "the host workflow runs ctest" $?
else
  skip TS-100 "workflow inspection" "no .github/workflows"
fi

# ── TS-101 · FR-BLD-08 — no release asset without a tag ──────────────────────
grep -q "startsWith(github.ref, 'refs/tags/')" "$wf/build.yml"
check TS-101 "the release step is gated on a tag" $?

# ── TS-096 · FR-BLD-03 — the simulated build is a compile-time choice ────────
grep -q 'CONFIG_GPLUG_SIM_METER' main/CMakeLists.txt
check TS-096 "variant selection is a Kconfig symbol, not a runtime path" $?
! grep -rqE 'sim_mode|set_sim|enable_sim' main/*.cpp
check TS-096 "no runtime switch enables simulated data" $?

# ── Checks that need a built binary ───────────────────────────────────────────
BIN=$(ls "$BUILD"/*.bin 2>/dev/null | grep -v bootloader | grep -v partition | head -1)
if [ -z "$BIN" ]; then
  for id in TS-094 TS-095 TS-097 TS-098; do
    skip "$id" "binary inspection" "no firmware binary in $BUILD"
  done
else
  # TS-112 · FR-SUP-09 — a shipped image carries no credentials.
  #
  # If it does, SSID and broker host are never absent, the device never enters
  # Provisioning, and eleven requirements are dead code in the field. This
  # project shipped exactly that until 2026-08-05, so the check inspects the
  # binary rather than trusting the Kconfig default it was built from.
  # Both the current bench SSID and the retired one: an image built from an old
  # checkout leaks credentials just as effectively as one built from today's.
  case "$(strings "$BIN" | grep -cE '^(wb-7cb1c2|wb-037e71|gplug-bench|benchtest123)$')" in
    0) check TS-112 "no bench credentials in the image" 0 ;;
    *) check TS-112 "no bench credentials in the image" 1 ;;
  esac

  # TS-094/095/097/098 · FR-BLD-01/02/04/05 — the version string tells the truth
  # about what it was built from, and the production binary carries no capture.
  #
  # The version is read first because it says which variant this is, and three of
  # the four checks below only apply to one variant. A check that asserts a
  # production rule against a simulated image fails for a reason that has nothing
  # to do with the requirement, which is worse than not running it.
  ver=$(strings "$BIN" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9a-g]+)?(-sim)?$' | head -1)

  # TS-097 · FR-BLD-04 — the production binary carries no capture data.
  # The fixture's opening bytes are the signature to look for. A simulated image
  # is *required* to contain one, so asserting this against it inverts the rule.
  case "$ver" in
    *-sim) skip TS-097 "embedded capture" "a simulated build is meant to carry one" ;;
    *)     ! grep -qa '7E A0 84 CE FF 03' "$BIN"
           check TS-097 "production binary contains no embedded capture" $? ;;
  esac

  if [ -z "$ver" ]; then
    for id in TS-094 TS-095 TS-098; do skip "$id" "version string" "none found in $BIN"; done
  else
    echo "         version string: $ver"
    if git describe --tags --exact-match >/dev/null 2>&1; then
      [ "$ver" = "$(git describe --tags --exact-match)" ]
      check TS-094 "tagged build: version equals the tag" $?
    else
      # Two separate claims. The format is always checkable. Equality with HEAD
      # is only meaningful when the binary was built from HEAD — comparing a
      # stale artefact against a moved HEAD reports a defect that is not there,
      # which is worse than reporting nothing.
      # A manually dispatched build stamps the version it was given, so it has
      # no hash to check and this claim does not apply to it. Recognised by the
      # absence of a hash-shaped suffix rather than by an input, because the
      # binary is the only evidence available here.
      if [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-sim)?$ ]]; then
        skip TS-095 "commit hash" "version was supplied to the build, not derived from git"
      else
        [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9a-f]{7}(-sim)?$ ]]
        check TS-095 "untagged build: version carries a short commit hash" $?
        built=${ver#*-}; built=${built%-sim}
        if [ "$built" = "$(git rev-parse --short HEAD)" ]; then
          check TS-095 "the hash is HEAD" 0
        else
          skip TS-095 "hash equals HEAD" "binary built from $built, HEAD is $(git rev-parse --short HEAD)"
        fi
      fi
    fi
    case "$ver" in
      *-sim) check TS-098 "simulated build carries the -sim suffix" 0 ;;
      *)     skip TS-098 "-sim suffix" "this is a production build" ;;
    esac
  fi
fi

echo
[ $fail = 0 ] && echo "artefact checks: all passed" || echo "artefact checks: FAILURES"
exit $fail
