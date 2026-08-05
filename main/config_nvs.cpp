// Configuration storage — the NVS side of config.h (FR-NVS-01..03).
//
// Deliberately thin. Everything that decides whether a record is usable lives
// in config.h as a pure core, so this file holds only the calls that need a
// flash partition and cannot run on a host.
#include "config.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace gplug {
namespace {

constexpr const char* TAG = "config";

// One namespace, four keys. NVS key names are capped at 15 characters, which is
// why these are abbreviated rather than spelled out.
constexpr const char* NAMESPACE = "gplug";
constexpr const char* KEY_SSID = "ssid";
constexpr const char* KEY_PASS = "pass";
constexpr const char* KEY_BROKER = "broker";
constexpr const char* KEY_HOST = "host";

// Read one string, leaving the destination empty when the key is absent.
// An absent key is not an error here: a record that is missing a field is
// judged by config_fault(), which reports *which* field and why. Treating it as
// a read failure would lose that distinction.
void read_str(nvs_handle_t h, const char* key, char* out, size_t cap) {
  size_t len = cap;
  if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

}  // namespace

// Storage is initialised before anything reads configuration.
//
// A partition that cannot be opened is erased and re-initialised rather than
// treated as fatal. This is the FR-NVS-02 path: a truncated or version-mismatched
// partition must leave the device running on defaults, not in a boot loop, and a
// device that will not boot in a meter cabinet needs a visit.
esp_err_t config_storage_init() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS unusable (%s) — erasing and starting from defaults",
             esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

// Load the stored configuration. Returns the fault, so the caller can say what
// was wrong rather than only that something was.
ConfigFault config_load(Config& out) {
  out = Config{};

  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    // No namespace at all is a first boot, not damage. It reads as a missing
    // SSID, which is exactly what it is.
    return ConfigFault::NoSsid;
  }

  read_str(h, KEY_SSID, out.ssid, sizeof(out.ssid));
  read_str(h, KEY_PASS, out.passphrase, sizeof(out.passphrase));
  read_str(h, KEY_BROKER, out.broker, sizeof(out.broker));
  read_str(h, KEY_HOST, out.hostname, sizeof(out.hostname));
  nvs_close(h);

  return config_fault(out);
}

// Store a configuration, all four keys or none.
//
// Committed once at the end: nvs_commit is what makes the write durable, and
// committing per key would leave a power cut able to store an SSID without its
// passphrase — a record that passes every field check and cannot associate.
bool config_save(const Config& c) {
  if (!config_usable(c)) {
    ESP_LOGE(TAG, "refusing to store an unusable configuration: %s",
             describe(config_fault(c)));
    return false;
  }

  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  bool ok = nvs_set_str(h, KEY_SSID, c.ssid) == ESP_OK &&
            nvs_set_str(h, KEY_PASS, c.passphrase) == ESP_OK &&
            nvs_set_str(h, KEY_BROKER, c.broker) == ESP_OK &&
            nvs_set_str(h, KEY_HOST, c.hostname) == ESP_OK &&
            nvs_commit(h) == ESP_OK;

  nvs_close(h);
  if (!ok) ESP_LOGE(TAG, "storing configuration failed");
  return ok;
}

// The configuration to run with: what was stored, or the build's defaults.
//
// The fallback exists because provisioning does not yet (FR-PRV-*). When it
// lands, an unusable record enters Provisioning instead of falling back, and
// this function is where that changes — the caller does not need to know which
// happened, only that what it receives is usable.
Config config_effective() {
  Config stored;
  const ConfigFault fault = config_load(stored);
  if (fault == ConfigFault::None) {
    ESP_LOGI(TAG, "configuration from NVS: ssid=%s broker=%s", stored.ssid, stored.broker);
    return stored;
  }

  ESP_LOGW(TAG, "stored configuration unusable (%s) — using build defaults",
           describe(fault));
  Config fallback{};
  std::strncpy(fallback.ssid, CONFIG_GPLUG_WIFI_SSID, sizeof(fallback.ssid) - 1);
  std::strncpy(fallback.passphrase, CONFIG_GPLUG_WIFI_PASSWORD,
               sizeof(fallback.passphrase) - 1);
  std::strncpy(fallback.broker, CONFIG_GPLUG_MQTT_URI, sizeof(fallback.broker) - 1);
  return fallback;
}

}  // namespace gplug
