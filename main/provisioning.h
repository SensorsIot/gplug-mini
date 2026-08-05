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

// **PROPOSED RULE — not yet accepted.** D-C6 is cited by FR-PRV-02 as the
// authority for "a documented rule" and does not state one, so this is a
// proposal to be confirmed or replaced, not a settled decision.
//
// The passphrase is the full MAC as twelve lowercase hex characters. Twelve
// clears the WPA2 minimum, it is derivable from a label carrying the MAC, and
// it needs nothing stored to recover.
//
// What it protects against, stated plainly because the alternative is a
// requirement that reads stronger than it is: **casual association only.** The
// MAC is broadcast in every beacon, so anyone within radio range who cares can
// read it and compute this. Against the threat profile in FSD §18.1 — a device
// in a private basement on a home network — that is the intended level. It is
// not confidentiality, and FR-SEC-03's "WPA2-protected" must not be read as
// claiming otherwise.
constexpr size_t ap_passphrase(const uint8_t mac[6], char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "%02x%02x%02x%02x%02x%02x",
                              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  if (static_cast<size_t>(n) < WPA2_MIN_PASSPHRASE) return 0;
  return static_cast<size_t>(n);
}

}  // namespace gplug
