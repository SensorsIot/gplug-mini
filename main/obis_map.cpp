#include "obis_map.h"

#include <cstring>

namespace gplug {
namespace {

// Every entry here was observed decoding from a published capture. Codes are
// added when a capture shows them, not when the standard lists them: a mapping
// nobody has seen data for is a guess wearing a table's clothes.
constexpr ObisEntry TABLE[] = {
    { "0.0.42.0.0.255",  "meter_serial",        "",    Kind::IDENTITY },
    { "0.0.96.1.0.255",  "meter_serial",        "",    Kind::IDENTITY },
    { "0.0.96.1.1.255",  "device_id_2",         "",    Kind::IDENTITY },
    { "0.0.1.0.0.255",   "meter_clock",         "",    Kind::TIMESTAMP },

    { "1.0.1.7.0.255",   "active_power_plus",   "W",   Kind::INSTANT },
    { "1.0.2.7.0.255",   "active_power_minus",  "W",   Kind::INSTANT },
    { "1.0.3.7.0.255",   "reactive_power_plus", "var", Kind::INSTANT },
    { "1.0.4.7.0.255",   "reactive_power_minus","var", Kind::INSTANT },
    { "1.0.13.7.0.255",  "power_factor",        "",    Kind::INSTANT },

    { "1.1.1.8.0.255",   "active_energy_plus",  "Wh",  Kind::CUMULATIVE },
    { "1.1.2.8.0.255",   "active_energy_minus", "Wh",  Kind::CUMULATIVE },
    { "1.1.5.8.0.255",   "reactive_energy_q1",  "varh",Kind::CUMULATIVE },
    { "1.1.6.8.0.255",   "reactive_energy_q2",  "varh",Kind::CUMULATIVE },
    { "1.1.7.8.0.255",   "reactive_energy_q3",  "varh",Kind::CUMULATIVE },
    { "1.1.8.8.0.255",   "reactive_energy_q4",  "varh",Kind::CUMULATIVE },
};

}  // namespace

const ObisEntry* obis_lookup(const char* obis) {
  for (const auto& e : TABLE) {
    if (std::strcmp(e.obis, obis) == 0) return &e;
  }
  return nullptr;
}

bool obis_is_identity(const char* obis) {
  const ObisEntry* e = obis_lookup(obis);
  return e != nullptr && e->kind == Kind::IDENTITY;
}

}  // namespace gplug
