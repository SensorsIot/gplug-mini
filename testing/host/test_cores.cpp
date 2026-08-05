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
#include "config.h"
#include "ha_discovery.h"
#include "indicator.h"
#include "obis_map.h"
#include "ota.h"
#include "provisioning.h"

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

// TS-026 — FR-AGG-03. One cycle produces one message.
//
// The rule is about the set, not the values: a device that published each value
// as it decoded would satisfy every other requirement here and still be wrong,
// because Home Assistant would see a household's registers arrive as eight
// separate states and graph the gaps between them.
void one_publish_per_cycle() {
  gplug::CycleSet set;
  check(set.empty(), "a fresh set holds nothing");

  set.add_integer("active_energy_plus", 25149419);
  set.add_real("active_power_plus", 777.0);
  check(set.size() == 2, "both values are in the one set");

  const std::string first = set.json();
  const std::string again = set.json();
  printf("       %s\n", first.c_str());
  check(first == again, "the set serialises to the same message when read twice");
  check(first.front() == '{' && first.back() == '}', "one complete JSON object, not a fragment");

  set.clear();
  check(set.empty(), "the set is empty again for the next cycle");
}

// TS-027 — FR-AGG-04. A register repeated within one cycle keeps its first
// value. The later one is discarded, and discarding is what must be asserted:
// both values are plausible readings, so an implementation that overwrites
// produces a message nothing downstream can tell is wrong.
void first_occurrence_retained() {
  gplug::CycleSet set;
  check(set.add_integer("active_energy_plus", 25149419), "the first occurrence is stored");
  check(!set.add_integer("active_energy_plus", 99999999), "the second is refused");
  check(set.size() == 1, "the register appears once in the set");

  // The rule is per register, not per cycle: a different register still lands.
  // Asserted before the set is serialised, because serialising closes it.
  check(set.add_real("active_power_plus", 777.0), "a different register is unaffected");

  const std::string json = set.json();
  printf("       %s\n", json.c_str());
  check(json.find("25149419") != std::string::npos, "the first value is the one published");
  check(json.find("99999999") == std::string::npos, "the later value is nowhere in the message");

  // Closing is one-way. A value arriving after the message was built would
  // otherwise be dropped in silence, which reads exactly like the duplicate
  // rule working and is a different bug entirely.
  check(!set.add_integer("late_arrival", 1), "a set that has been serialised takes nothing more");
}

// TS-029 — FR-AGG-06. A cycle that decoded nothing publishes nothing.
//
// The prohibited outcome is the point. An empty or all-zero payload is worse
// than silence, because Home Assistant records it as a reading and a zeroed
// cumulative register reads as a household that consumed nothing.
void empty_set_not_published() {
  gplug::CycleSet set;
  check(set.empty(), "noise decoded to no values, so the set is empty");
  check(std::string(set.json()).empty(), "an empty set yields no message at all — not '{}'");

  set.add_integer("active_energy_plus", 0);
  check(!set.empty(), "a set holding a genuine zero is not an empty set");
  printf("       %s\n", set.json());
  check(std::string(set.json()) == "{\"active_energy_plus\":0}",
        "a real zero reading is published — only the absence of values is suppressed");
}

// TS-102 — FR-NVS-02. A stored configuration that cannot be used is detected
// and named, so the device enters Provisioning instead of attempting a
// connection that cannot succeed.
//
// The target-tier test corrupts a partition and reboots, which proves the
// device survives it. This proves the thing that decides — and it can cover
// every malformed record the flash might hand back, which reflashing cannot.
void config_validity() {
  using gplug::Config;
  using gplug::ConfigFault;
  using gplug::config_fault;
  using gplug::config_usable;

  Config good{};
  std::strcpy(good.ssid, "gplug-bench");
  std::strcpy(good.passphrase, "benchtest123");
  std::strcpy(good.broker, "mqtt://10.42.0.1:1883");
  check(config_usable(good), "a complete record is usable");

  // An erased namespace reads back as zeros, which is first boot rather than
  // corruption. The device must tell those apart: one is normal and one is not.
  Config blank{};
  check(config_fault(blank) == ConfigFault::NoSsid, "an empty record reports a missing SSID");

  // An open network has no passphrase. Requiring one would make a valid
  // deployment unconfigurable, and the failure would look like bad credentials.
  Config open_net = good;
  open_net.passphrase[0] = '\0';
  check(config_usable(open_net), "an open network needs no passphrase");

  // A bare address is the mistake a person makes at a portal, and it fails far
  // from its cause: the MQTT client reports a connect error, not a bad URI.
  Config bare = good;
  std::strcpy(bare.broker, "10.42.0.1");
  check(config_fault(bare) == ConfigFault::BrokerMalformed,
        "a broker address without a scheme is rejected here, not at connect time");

  Config no_broker = good;
  no_broker.broker[0] = '\0';
  check(config_fault(no_broker) == ConfigFault::NoBroker, "a missing broker is reported");

  // Length limits are the protocol's. A longer value did not come from our
  // portal, so the record is corrupt rather than merely wrong.
  Config long_ssid = good;
  std::memset(long_ssid.ssid, 'a', gplug::SSID_MAX + 1);
  long_ssid.ssid[gplug::SSID_MAX + 1] = '\0';
  check(config_fault(long_ssid) == ConfigFault::SsidTooLong,
        "an SSID longer than 802.11 permits is a corrupt record");

  // Boundary either side, because a limit tested only well inside its range
  // passes with the comparison inverted.
  Config max_ssid = good;
  std::memset(max_ssid.ssid, 'a', gplug::SSID_MAX);
  max_ssid.ssid[gplug::SSID_MAX] = '\0';
  check(config_usable(max_ssid), "an SSID of exactly 32 octets is valid");

  for (const char* bad : { "-leading", "trailing-", "has space", "under_score" }) {
    Config h = good;
    std::strcpy(h.hostname, bad);
    check(config_fault(h) == ConfigFault::HostnameInvalid, bad);
  }
  Config h = good;
  std::strcpy(h.hostname, "gplug-01");
  check(config_usable(h), "a valid DNS label is accepted");

  printf("       empty record reports: %s\n", gplug::describe(config_fault(blank)));
}

// TS-104 — FR-PRV-02, FR-SEC-03. The SoftAP's name and passphrase are a
// function of the MAC, so an owner can recover access from a label.
//
// The bench test associates with the derived passphrase and then a wrong one,
// which proves WPA2 is on. It cannot show the *rule* is right — it computes the
// passphrase the same way the firmware does, so a wrong rule agrees with itself
// and the test passes. That is what this pins.
void ap_identity() {
  const uint8_t mac[6] = { 0xb0, 0x81, 0x84, 0x25, 0x22, 0x5c };
  char ssid[gplug::AP_SSID_MAX + 1];
  char pass[gplug::AP_PASSPHRASE_MAX + 1];

  check(gplug::ap_ssid(mac, ssid, sizeof(ssid)) > 0, "SSID built");
  printf("       ssid = %s\n", ssid);
  check(std::string(ssid) == "gplug-25225c", "SSID is gplug- plus the last three octets");

  const size_t n = gplug::ap_passphrase(mac, pass, sizeof(pass));
  printf("       passphrase = %s (%zu chars)\n", pass, n);
  check(n >= gplug::WPA2_MIN_PASSPHRASE, "passphrase clears the WPA2 minimum");
  check(std::string(pass) == "gplug25225c",
        "passphrase is gplug plus the octets the SSID shows — derivable from the scan list");

  // Two devices must not share a passphrase. Obvious, and exactly the property
  // a truncated or mis-indexed rule breaks while still producing valid output.
  const uint8_t other[6] = { 0xb0, 0x81, 0x84, 0x25, 0x22, 0x5d };
  char pass2[gplug::AP_PASSPHRASE_MAX + 1];
  gplug::ap_passphrase(other, pass2, sizeof(pass2));
  check(std::string(pass) != std::string(pass2), "different MACs give different passphrases");

  // A buffer too small reports failure rather than truncating. A truncated
  // passphrase is still a valid WPA2 passphrase, so it would be accepted by
  // ap_start and simply never match what the owner was told.
  char tiny[6];
  check(gplug::ap_passphrase(mac, tiny, sizeof(tiny)) == 0,
        "a passphrase that does not fit reports failure, never a short one");
}

// TS-105 — FR-LED-05, FSD §11.3. No two indications look the same, and each
// matches the table rather than something plausible.
//
// The second half matters more than the first. A set of patterns invented to be
// merely distinct is self-consistent, and a test written from that code asserts
// the invention — which is exactly what the first draft of this file did, with
// a 1 s Connecting blink the spec puts at 5 s and a Updating pattern the spec
// says alternates blue and green.
void indications_distinguishable() {
  using gplug::Indication;
  using gplug::pattern_for;

  const Indication all[] = { Indication::Boot, Indication::Provisioning,
                             Indication::Connecting, Indication::Linked,
                             Indication::Operational, Indication::Updating };
  for (auto a : all) {
    for (auto b : all) {
      if (a == b) continue;
      char what[96];
      std::snprintf(what, sizeof(what), "indication %d differs from %d",
                    static_cast<int>(a), static_cast<int>(b));
      check(gplug::distinguishable(a, b), what);
    }
  }

  // Each row of §11.3, read back.
  const auto prov = pattern_for(Indication::Provisioning);
  check(prov.a_blue && !prov.a_red && !prov.a_green && prov.period_ms == 0,
        "PROVISIONING is blue steady");

  const auto conn = pattern_for(Indication::Connecting);
  check(conn.a_red && conn.period_ms == 5000, "CONNECTING is a red blink, 5 s period");

  const auto linked = pattern_for(Indication::Linked);
  check(linked.a_red && linked.period_ms == 1000, "LINKED is a red blink, 1 s period");

  const auto oper = pattern_for(Indication::Operational);
  check(!oper.a_red && !oper.a_green && !oper.a_blue && oper.period_ms == 0,
        "OPERATIONAL is off between publications");
  check(gplug::PUBLISH_PULSE_MS == 100, "the publication pulse is 100 ms");

  const auto upd = pattern_for(Indication::Updating);
  check(upd.a_blue && upd.b_green && upd.period_ms == 200,
        "UPDATING alternates blue and green at 200 ms");

  // The two red blinks differ only in rate, so the rate is what must hold. An
  // edit making them equal leaves two states indistinguishable on the board
  // while every colour assertion above still passes.
  check(conn.period_ms != linked.period_ms,
        "CONNECTING and LINKED blink at different rates — colour alone cannot separate them");
}

// TS-106 — FR-OTA-01, FR-OTA-02. What the device will and will not download
// from, decided before anything is fetched.
//
// The command topic carries the one input a person types by hand, and a
// malformed value that reaches the downloader surfaces as a transport error
// minutes later with nothing tying it back to the message. So the refusal is
// worth more than the acceptance, and both are asserted here.
void ota_url_rules() {
  using gplug::ota_url_acceptable;

  check(ota_url_acceptable("http://10.42.0.1:8080/firmware/gplug/app.bin"),
        "the bench relay's URL is accepted");
  check(ota_url_acceptable("https://example.com/a.bin"), "https is accepted");

  check(!ota_url_acceptable(nullptr), "no payload is not a URL");
  check(!ota_url_acceptable(""), "an empty payload is not a URL");
  check(!ota_url_acceptable("10.42.0.1/app.bin"), "an address with no scheme is refused");
  check(!ota_url_acceptable("ftp://example.com/a.bin"), "a scheme we cannot fetch is refused");
  check(!ota_url_acceptable("http://"), "a scheme with nothing after it is refused");

  // Whitespace and control characters are the visible signature of a truncated
  // or line-wrapped payload — the shape a URL takes when it has been pasted
  // through something that reflowed it.
  check(!ota_url_acceptable("http://example.com/a.bin\n"), "a trailing newline is refused");
  check(!ota_url_acceptable("http://exa mple.com/a.bin"), "an embedded space is refused");

  // Longer than any image URL needs. An unbounded payload copied into a fixed
  // buffer is the one failure here that is not merely inconvenient.
  std::string huge = "http://example.com/";
  huge.append(300, 'a');
  check(!ota_url_acceptable(huge.c_str()), "an overlong URL is refused, not truncated");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: test_cores <case>\n"); return 2; }
  const std::string name = argv[1];
  printf("%s\n", name.c_str());

  if (name == "cycle") cycle_boundary();
  else if (name == "obis") obis_mapping();
  else if (name == "discovery") discovery_payloads();
  else if (name == "oneset") one_publish_per_cycle();
  else if (name == "firstwins") first_occurrence_retained();
  else if (name == "emptyset") empty_set_not_published();
  else if (name == "config")   config_validity();
  else if (name == "apident")  ap_identity();
  else if (name == "leds")     indications_distinguishable();
  else if (name == "otaurl")   ota_url_rules();
  else { std::fprintf(stderr, "unknown case: %s\n", name.c_str()); return 2; }

  printf("%s: %d failure(s)\n", name.c_str(), failures);
  return failures == 0 ? 0 : 1;
}
