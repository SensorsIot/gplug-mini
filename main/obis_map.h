// OBIS code → label, unit, kind. A pure core.
//
// Getting this wrong publishes a real value under the wrong name, which is
// harder to notice than no value at all. `kind` exists because cumulative
// registers must not travel as floats (FR-DEC-04) and because Home Assistant
// treats a total and an instantaneous reading differently.
#pragma once

#include <cstdint>

namespace gplug {

enum class Kind : uint8_t {
  UNKNOWN,
  IDENTITY,       // meter serial — a string, not a measurement
  INSTANT,        // power now; float is fine
  CUMULATIVE,     // energy since forever; integers only
  TIMESTAMP,
};

struct ObisEntry {
  const char* obis;
  const char* label;
  const char* unit;
  Kind kind;
};

// Returns nullptr for a code this build does not publish. Absence is not an
// error: the meter's register set is a deployment choice, and publishing a
// placeholder for an absent register would make "missing" and "zero"
// indistinguishable (FR-HA-05).
const ObisEntry* obis_lookup(const char* obis);

// True for the two codes an E450 may carry its identity under. One published
// configuration uses the COSEM logical device name, the other device ID 1, and a
// decoder that knows only one finds nothing on the other meter (FR-MTR-10).
bool obis_is_identity(const char* obis);

}  // namespace gplug
