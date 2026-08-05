// Provisioning — the SoftAP's identity, as a pure core (FR-PRV-01, FR-PRV-02,
// FR-SEC-03).
//
// The network's name and passphrase are a function of the MAC and nothing else,
// so they are decided here and tested at host tier. The SoftAP, the DNS
// redirect and the configuration page are ESP-IDF and live in provisioning.cpp.
//
// Deriving both from the MAC is what lets an owner recover access from a label
// on the device rather than from a value only the firmware knows.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace gplug {

constexpr size_t AP_SSID_MAX = 32;
constexpr size_t AP_PASSPHRASE_MAX = 63;

// WPA2 will not accept anything shorter. Stated as a constant because the rule
// below has to satisfy it, and a rule that quietly produced seven characters
// would fail at ap_start with an error about the passphrase rather than about
// the rule that made it.
constexpr size_t WPA2_MIN_PASSPHRASE = 8;

// `gplug-XXYYZZ` from the last three octets — the form that appears in a scan
// list, and the part an owner can match against a label.
constexpr size_t ap_ssid(const uint8_t mac[6], char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "gplug-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// `gplug` followed by the same three octets the SSID carries — eleven
// characters, which clears the WPA2 minimum (D-C6).
//
// Derived from the octets that appear in the SSID, not from the full MAC,
// because D-C6 requires it to be recoverable *without a label*. An owner reads
// `gplug-25225c` in the scan list and types `gplug25225c`; a rule using the
// three octets the SSID does not show would need a sticker, and a device in a
// meter cabinet loses stickers.
//
// So it is not a secret, and is not meant to be. What it buys, stated plainly
// because the alternative is a requirement that reads stronger than it is:
// WPA2 encrypts the association, and the network refuses a client that does not
// know the rule at all. Against §18.1's threat profile — a private basement, a
// home network — that is the intended level. FR-SEC-03's "WPA2-protected" must
// not be read as claiming confidentiality against anyone in radio range.
constexpr size_t ap_passphrase(const uint8_t mac[6], char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "gplug%02x%02x%02x", mac[3], mac[4], mac[5]);
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  if (static_cast<size_t>(n) < WPA2_MIN_PASSPHRASE) return 0;
  return static_cast<size_t>(n);
}

#ifndef GPLUG_HOST_TEST
// Runs Provisioning to completion: SoftAP up, page served, DNS redirected.
// Returns true when a configuration was submitted and stored, false when the
// timeout expired with nothing submitted — in which case whatever was already
// stored is untouched (FR-PRV-06).
//
// Blocking on purpose. Provisioning is not a background activity: nothing else
// the device does is meaningful until it knows which network to join.
bool provisioning_run();
#endif

}  // namespace gplug
