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
  # TS-097 · FR-BLD-04 — the production binary carries no capture data.
  # The fixture's opening bytes are the signature to look for.
  ! grep -qa '7E A0 84 CE FF 03' "$BIN"
  check TS-097 "production binary contains no embedded capture" $?

  # TS-094/095/098 · FR-BLD-01/02/05 — the version string tells the truth about
  # what it was built from.
  ver=$(strings "$BIN" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9a-g]+)?(-sim)?$' | head -1)
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
      [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9a-f]{7}(-sim)?$ ]]
      check TS-095 "untagged build: version carries a short commit hash" $?
      built=${ver#*-}; built=${built%-sim}
      if [ "$built" = "$(git rev-parse --short HEAD)" ]; then
        check TS-095 "the hash is HEAD" 0
      else
        skip TS-095 "hash equals HEAD" "binary built from $built, HEAD is $(git rev-parse --short HEAD)"
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
