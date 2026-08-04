// Network: WiFi station and the MQTT client.
//
// Both retry indefinitely and neither resets to clear a fault (FR-ERR-04). The
// station never falls back to SoftAP on a failure to connect — a device in a
// basement that puts up a portal nobody can see is unreachable, not recoverable
// (FR-SUP-04).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gplug {

// Blocks until the station has an address. Retries for as long as it takes.
void wifi_start_and_wait();

// Colon-separated lower case, from the station interface. Identifies the MQTT
// client and appears as a Home Assistant device connection — never as the key
// for entity identity, which belongs to the meter serial.
const char* wifi_mac();

// Connects and publishes availability. The last will is registered before
// connecting, so a power cut marks the device offline without its cooperation
// (FR-HA-06).
void mqtt_start();

bool mqtt_connected();

// Retained, QoS 1 — the discovery contract in FSD Appendix D.
void mqtt_publish_discovery(const char* topic, const char* payload);

// Not retained, QoS 0: a retained measurement is indistinguishable from a
// current one, and the next subscriber would be handed a stale reading as
// though the meter had just sent it (FR-HA-07).
void mqtt_publish_state(const char* payload);

}  // namespace gplug
