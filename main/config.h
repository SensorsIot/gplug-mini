// Device configuration — the record, its validity rules, and its defaults.
//
// A pure core (FR-NVS-01..03). The storage backend is ESP-IDF NVS and lives in
// config_nvs.cpp; everything that decides *whether a configuration is usable*
// is here, so it can be tested at host tier against every malformed record the
// flash can hand back rather than by corrupting a partition and rebooting.
//
// That split matters because FR-NVS-02's failure path is the one that must not
// be reached by guesswork: a device that treats a half-written record as valid
// joins nothing and reports no reason.
#pragma once

#include <cstddef>
#include <cstring>

namespace gplug {

// Field limits are the protocol's, not ours. 802.11 caps the SSID at 32 octets
// and WPA2 passphrases at 63 characters; a longer stored value is a corrupt
// record rather than a configuration to attempt.
constexpr size_t SSID_MAX = 32;
constexpr size_t PASSPHRASE_MAX = 63;
constexpr size_t BROKER_MAX = 127;
constexpr size_t HOSTNAME_MAX = 32;

struct Config {
  char ssid[SSID_MAX + 1]{};
  char passphrase[PASSPHRASE_MAX + 1]{};
  char broker[BROKER_MAX + 1]{};
  char hostname[HOSTNAME_MAX + 1]{};
};

// Why a stored record cannot be used. Reported rather than reduced to a bool,
// because the device must say which — an absent record is a first boot and an
// invalid one is corruption or a downgrade, and only the second is worth
// alarming about.
enum class ConfigFault {
  None,
  NoSsid,           // never provisioned, or the namespace was cleared
  SsidTooLong,      // longer than 802.11 allows — the record is not ours
  PassphraseTooLong,
  NoBroker,
  BrokerMalformed,  // no scheme; an address alone is not a broker URI
  HostnameInvalid,
};

constexpr const char* describe(ConfigFault f) {
  switch (f) {
    case ConfigFault::None:              return "valid";
    case ConfigFault::NoSsid:            return "no SSID stored";
    case ConfigFault::SsidTooLong:       return "SSID longer than 802.11 permits";
    case ConfigFault::PassphraseTooLong: return "passphrase longer than WPA2 permits";
    case ConfigFault::NoBroker:          return "no broker URI stored";
    case ConfigFault::BrokerMalformed:   return "broker URI has no scheme";
    case ConfigFault::HostnameInvalid:   return "hostname is not a valid DNS label";
  }
  return "unknown";
}

// A hostname must be a DNS label: letters, digits and hyphens, not starting or
// ending with a hyphen. Checked because it is published to mDNS and to the
// broker, and an invalid one fails there rather than here — far from its cause.
constexpr bool valid_hostname(const char* h) {
  if (h == nullptr || *h == '\0') return true;   // absent is allowed; a default is used
  const size_t n = std::strlen(h);
  if (n > HOSTNAME_MAX) return false;
  if (h[0] == '-' || h[n - 1] == '-') return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = h[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

// The record's fault, or None. A device with a fault enters Provisioning
// (FR-NVS-02) rather than attempting a connection that cannot succeed.
//
// The passphrase is deliberately not required: an open network has none, and
// demanding one would make a valid deployment unconfigurable.
constexpr ConfigFault config_fault(const Config& c) {
  if (c.ssid[0] == '\0') return ConfigFault::NoSsid;
  if (std::strlen(c.ssid) > SSID_MAX) return ConfigFault::SsidTooLong;
  if (std::strlen(c.passphrase) > PASSPHRASE_MAX) return ConfigFault::PassphraseTooLong;
  if (c.broker[0] == '\0') return ConfigFault::NoBroker;
  if (std::strstr(c.broker, "://") == nullptr) return ConfigFault::BrokerMalformed;
  if (!valid_hostname(c.hostname)) return ConfigFault::HostnameInvalid;
  return ConfigFault::None;
}

constexpr bool config_usable(const Config& c) {
  return config_fault(c) == ConfigFault::None;
}

}  // namespace gplug
