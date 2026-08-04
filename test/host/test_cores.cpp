// The pure cores, compiled straight from main/ — the same source the firmware
// builds. No ESP-IDF, no hardware, microseconds.
//
// Both cores take their inputs as arguments and return values, which is what
// lets the 2000 ms rule be tested without waiting two seconds and the OBIS
// table be tested without a meter.

#include <cstdio>
#include <cstring>
#include <string>

#include "aggregator.h"
#include "obis_map.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

// TS-HOST-05 — FR-AGG-01. The boundary is a gap of at least 2000 ms with
// something buffered. Asserted either side of the threshold, because a rule
// tested only well inside its range passes with the comparison inverted.
void cycle_boundary() {
  using gplug::cycle_ended;
  check(!cycle_ended(1999, 0, 10), "1999 ms is not yet a boundary");
  check(cycle_ended(2000, 0, 10), "2000 ms is a boundary");
  check(cycle_ended(2001, 0, 10), "2001 ms is a boundary");
  // Silence with nothing buffered is just silence. Without this, a quiet meter
  // publishes an empty set every two seconds.
  check(!cycle_ended(999999, 0, 0), "silence with an empty buffer is not a cycle");
}

// TS-HOST-06 — FR-DEC-04, FR-MTR-09, FR-HA-05. What the map must get right:
// energy is cumulative, power is not, the identity lives under either of two
// codes, and an unknown code returns nothing rather than a placeholder.
void obis_mapping() {
  using namespace gplug;
  const ObisEntry* energy = obis_lookup("1.1.1.8.0.255");
  check(energy != nullptr && energy->kind == Kind::CUMULATIVE,
        "active energy is cumulative — must not travel as a float");
  check(energy != nullptr && std::strcmp(energy->unit, "Wh") == 0, "energy is in Wh");

  const ObisEntry* power = obis_lookup("1.0.1.7.0.255");
  check(power != nullptr && power->kind == Kind::INSTANT, "active power is instantaneous");

  check(obis_is_identity("0.0.42.0.0.255"), "identity via COSEM logical device name");
  check(obis_is_identity("0.0.96.1.0.255"), "identity via device ID 1");
  check(!obis_is_identity("1.0.1.7.0.255"), "a measurement is not an identity");

  check(obis_lookup("9.9.9.9.9.255") == nullptr,
        "an unmapped code returns nothing — absent and zero are different facts");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: test_cores <case>\n"); return 2; }
  const std::string name = argv[1];
  printf("%s\n", name.c_str());

  if (name == "cycle") cycle_boundary();
  else if (name == "obis") obis_mapping();
  else { std::fprintf(stderr, "unknown case: %s\n", name.c_str()); return 2; }

  printf("%s: %d failure(s)\n", name.c_str(), failures);
  return failures == 0 ? 0 : 1;
}
