#include "ha_discovery.h"

#include <cstdio>

namespace gplug {
namespace {

// snprintf returns what it *would* have written, so a truncated payload reports
// a length past the buffer. Publishing truncated JSON creates an entity Home
// Assistant cannot parse and never retries, so treat truncation as failure.
size_t written(int n, size_t cap) {
  return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

struct Classes {
  const char* device_class;
  const char* state_class;
};

// FR-HA-04. `total_increasing` is what makes the Energy Dashboard willing to
// treat a cumulative register as consumption; `measurement` is what makes a
// power reading graph rather than accumulate.
Classes classes_for(Kind kind) {
  switch (kind) {
    case Kind::CUMULATIVE: return { "energy", "total_increasing" };
    case Kind::INSTANT:    return { "power", "measurement" };
    default:               return { nullptr, nullptr };
  }
}

}  // namespace

size_t discovery_topic(char* out, size_t cap, const char* serial, const char* label) {
  return written(std::snprintf(out, cap, "homeassistant/sensor/%s_%s/config",
                               serial, label), cap);
}

size_t state_topic(char* out, size_t cap, const char* mac) {
  return written(std::snprintf(out, cap, "gplug/%s/state", mac), cap);
}

size_t status_topic(char* out, size_t cap, const char* mac) {
  return written(std::snprintf(out, cap, "gplug/%s/status", mac), cap);
}

size_t discovery_payload(char* out, size_t cap, const ObisEntry& entry,
                         const char* serial, const char* mac) {
  const Classes c = classes_for(entry.kind);
  if (c.device_class == nullptr) return 0;   // identity and clock are not sensors

  char state[96], status[96];
  if (state_topic(state, sizeof(state), mac) == 0) return 0;
  if (status_topic(status, sizeof(status), mac) == 0) return 0;

  // One device object per meter so the entities group in Home Assistant, keyed
  // on the serial for the same reason unique_id is.
  const int n = std::snprintf(
      out, cap,
      "{"
      "\"name\":\"%s\","
      "\"unique_id\":\"%s_%s\","
      "\"state_topic\":\"%s\","
      "\"availability_topic\":\"%s\","
      "\"payload_available\":\"online\","
      "\"payload_not_available\":\"offline\","
      "\"value_template\":\"{{ value_json.%s }}\","
      "\"unit_of_measurement\":\"%s\","
      "\"device_class\":\"%s\","
      "\"state_class\":\"%s\","
      "\"device\":{"
      "\"identifiers\":[\"gplug_%s\"],"
      "\"name\":\"gPlug-mini %s\","
      "\"manufacturer\":\"Landis+Gyr\","
      "\"model\":\"E450\","
      "\"connections\":[[\"mac\",\"%s\"]]"
      "}"
      "}",
      entry.label, serial, entry.label, state, status,
      entry.label, entry.unit, c.device_class, c.state_class,
      serial, serial, mac);
  return written(n, cap);
}

}  // namespace gplug
