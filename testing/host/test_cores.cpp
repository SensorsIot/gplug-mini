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
#include "ha_discovery.h"
#include "obis_map.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

// TS-006 — FR-AGG-01. The boundary is a gap of at least 2000 ms with
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

// TS-007 — FR-DEC-04, FR-MTR-09, FR-HA-05. What the map must get right:
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

// TS-008 — FR-HA-02, FR-HA-04. The two properties that decide whether an
// entity is usable rather than merely created: the Energy Dashboard requires
// energy sensors to carry device_class energy and state_class total_increasing,
// and unique_id must key on the meter serial so history survives replacing the
// hardware.
void discovery_payloads() {
  using namespace gplug;
  char buf[768];

  const ObisEntry* energy = obis_lookup("1.1.1.8.0.255");
  check(discovery_payload(buf, sizeof(buf), *energy, "44337811", "b0:81:84:25:22:5c") > 0,
        "energy payload built");
  const std::string e(buf);
  check(e.find("\"device_class\":\"energy\"") != std::string::npos,
        "energy carries device_class energy");
  check(e.find("\"state_class\":\"total_increasing\"") != std::string::npos,
        "energy carries state_class total_increasing — the Energy Dashboard needs it");
  check(e.find("\"unique_id\":\"44337811_active_energy_plus\"") != std::string::npos,
        "unique_id keys on the meter serial, not the MAC");
  check(e.find("b0:81:84:25:22:5c") != std::string::npos, "MAC appears as a device connection");

  const ObisEntry* power = obis_lookup("1.0.1.7.0.255");
  check(discovery_payload(buf, sizeof(buf), *power, "44337811", "aa:bb") > 0, "power payload built");
  const std::string p(buf);
  check(p.find("\"state_class\":\"measurement\"") != std::string::npos,
        "power measures rather than accumulates");

  // An identity is not a sensor. Publishing one as a measurement entity puts a
  // serial number on a graph.
  const ObisEntry* identity = obis_lookup("0.0.96.1.0.255");
  check(discovery_payload(buf, sizeof(buf), *identity, "44337811", "aa:bb") == 0,
        "identity produces no sensor payload");

  // Truncation must fail rather than publish half a JSON document, which Home
  // Assistant cannot parse and never retries.
  char tiny[40];
  check(discovery_payload(tiny, sizeof(tiny), *energy, "44337811", "aa:bb") == 0,
        "a payload that does not fit reports failure, not truncation");

  char topic[128];
  check(discovery_topic(topic, sizeof(topic), "44337811", "active_energy_plus") > 0 &&
        std::string(topic) == "homeassistant/sensor/44337811_active_energy_plus/config",
        "discovery topic matches Appendix D");
  check(state_topic(topic, sizeof(topic), "aa:bb") > 0 &&
        std::string(topic) == "gplug/aa:bb/state", "state topic matches Appendix D");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: test_cores <case>\n"); return 2; }
  const std::string name = argv[1];
  printf("%s\n", name.c_str());

  if (name == "cycle") cycle_boundary();
  else if (name == "obis") obis_mapping();
  else if (name == "discovery") discovery_payloads();
  else { std::fprintf(stderr, "unknown case: %s\n", name.c_str()); return 2; }

  printf("%s: %d failure(s)\n", name.c_str(), failures);
  return failures == 0 ? 0 : 1;
}
