// Home Assistant MQTT discovery payloads — a pure core.
//
// Gets one thing right that is expensive to get wrong: the Energy Dashboard
// only accepts a sensor carrying `device_class: energy` with
// `state_class: total_increasing`. An entity published without them is created,
// looks fine, and is silently unusable for the one purpose it exists for.
//
// No esp_ call, no allocation, no clock: the caller supplies the buffer.
#pragma once

#include <cstddef>

#include "obis_map.h"

namespace gplug {

// `homeassistant/sensor/<serial>_<label>/config` (FSD Appendix D).
// Returns the length written, or 0 if the buffer was too small.
size_t discovery_topic(char* out, size_t cap, const char* serial, const char* label);

// The retained discovery payload for one entity.
//
// `unique_id` derives from the meter serial, not the MAC, so that history
// survives replacing the hardware (FR-HA-02). The MAC identifies the *client*,
// which is a different question and answered elsewhere.
size_t discovery_payload(char* out, size_t cap, const ObisEntry& entry,
                         const char* serial, const char* mac);

// `gplug/<mac>/state` and `gplug/<mac>/status` (FSD Appendix D).
size_t state_topic(char* out, size_t cap, const char* mac);
size_t status_topic(char* out, size_t cap, const char* mac);

}  // namespace gplug
