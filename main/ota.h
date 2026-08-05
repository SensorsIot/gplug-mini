// Firmware update over HTTP, triggered by MQTT (FR-OTA-01..09).
//
// Manual trigger only. There is no version check, no polling and no schedule:
// FR-OTA-02 forbids every trigger except a URL arriving on the command topic,
// because a meter reader that updates itself unattended can take a household's
// energy history offline while nobody is watching (D-U2).
#pragma once

#include <cstddef>

namespace gplug {

// Whether a string is a URL this device will download from.
//
// A pure check so it can be tested without a server. It exists because the
// command topic is the one input a person types by hand, and the failure it
// prevents is expensive: a malformed URL that reaches esp_https_ota surfaces as
// a download error minutes later, long after the message that caused it.
constexpr bool ota_url_acceptable(const char* url) {
  if (url == nullptr) return false;
  size_t n = 0;
  while (url[n] != '\0') {
    // Control characters and spaces have no place in a URL and are the visible
    // sign of a truncated or wrapped payload.
    if (static_cast<unsigned char>(url[n]) <= 0x20) return false;
    if (++n > 255) return false;   // longer than any image URL this needs
  }
  if (n < 12) return false;

  const bool http = url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p';
  if (!http) return false;
  // Both schemes are accepted here; FR-OTA-03's certificate check is what makes
  // https meaningful and is enforced at download time, not by inspecting a
  // prefix. Rejecting http outright would be a different requirement than the
  // one written, and the bench relay serves plain http.
  const size_t scheme = (url[4] == 's') ? 5 : 4;
  return url[scheme] == ':' && url[scheme + 1] == '/' && url[scheme + 2] == '/' &&
         url[scheme + 3] != '\0';
}

#ifndef GPLUG_HOST_TEST
// Subscribes to the command topic and services updates. Call once, after MQTT.
void ota_start();

// Called from the MQTT event handler when a message arrives on the command
// topic. Returns false when the payload is not a URL this device will act on —
// the caller logs it, because an ignored command must not look like silence.
bool ota_handle_command(const char* payload, size_t len);

// FR-OTA-05/06: marks the running image valid. Called once an MQTT session is
// established, and never on a successful boot alone — an image that boots and
// cannot reach the broker is exactly the image rollback exists for.
void ota_mark_valid_on_session();

// True while a download is in progress, so the indicator can show it.
bool ota_in_progress();
#endif

}  // namespace gplug
