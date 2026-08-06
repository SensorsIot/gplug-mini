// Network: WiFi station and the MQTT client.
//
// Both retry indefinitely and neither resets to clear a fault (FR-WDT-05). The
// station never falls back to SoftAP on a failure to connect — a device in a
// basement that puts up a portal nobody can see is unreachable, not recoverable
// (FR-SUP-04).
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace gplug {

// Brings up esp_netif and the default event loop, once.
//
// Both the station path and Provisioning need them before they may create a
// netif, and Provisioning runs first on a device with no configuration. Having
// each caller do it separately is how the portal came to call
// esp_netif_create_default_wifi_ap() on an uninitialised stack and abort at
// boot — a defect that compiled cleanly and could only be found by running it.
//
// Safe to call twice: an already-initialised stack reports ESP_ERR_INVALID_STATE
// and that is treated as success, because it is.
void net_stack_init();

// Blocks until the station has an address. Retries for as long as it takes.
// Takes the configuration rather than reading Kconfig, so a provisioned device
// and a bench build follow the same path.
void wifi_start_and_wait(const Config& conf);

// Colon-separated lower case, from the station interface. Identifies the MQTT
// client and appears as a Home Assistant device connection — never as the key
// for entity identity, which belongs to the meter serial.
const char* wifi_mac();

// Connects and publishes availability. The last will is registered before
// connecting, so a power cut marks the device offline without its cooperation
// (FR-HA-06).
void mqtt_start(const Config& conf);

bool mqtt_connected();

// Increments once per established broker session, so a caller can tell a new
// session from a continuing one. Anything the device must state to the broker
// rather than merely send it — retained discovery above all — has to be
// restated on a session it has not spoken on yet.
uint32_t mqtt_session();

// Retained, QoS 1 — the discovery contract in FSD Appendix D.
void mqtt_publish_discovery(const char* topic, const char* payload);

// Not retained, QoS 0: a retained measurement is indistinguishable from a
// current one, and the next subscriber would be handed a stale reading as
// though the meter had just sent it (FR-HA-07).
void mqtt_publish_state(const char* payload);

}  // namespace gplug
