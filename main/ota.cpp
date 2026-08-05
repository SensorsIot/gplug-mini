// Firmware update — download to the inactive slot, validate on a broker session.
#include "ota.h"

#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "indicator.h"

namespace gplug {
namespace {

constexpr const char* TAG = "ota";

char pending_url[256] = "";
volatile bool downloading = false;
volatile bool validated = false;
TaskHandle_t worker = nullptr;

void download_task(void*) {
  downloading = true;
  indicate(Indication::Updating);
  ESP_LOGI(TAG, "downloading %s", pending_url);

  esp_http_client_config_t http = {};
  http.url = pending_url;
  // FR-OTA-03. The bundle is ESP-IDF's certificate store, so a server whose
  // chain does not verify aborts here rather than delivering an image. Without
  // this an https URL would be transport encryption with no identity behind it,
  // which is the failure mode that looks secure in a log.
  http.crt_bundle_attach = esp_crt_bundle_attach;
  http.keep_alive_enable = true;
  http.timeout_ms = 20000;

  esp_https_ota_config_t cfg = {};
  cfg.http_config = &http;

  // esp_https_ota writes to the inactive slot and never to the running one
  // (FR-OTA-04) — the partition is chosen by esp_ota_get_next_update_partition,
  // not by us, so there is no opportunity to name the wrong one.
  const esp_err_t err = esp_https_ota(&cfg);

  downloading = false;
  if (err == ESP_OK) {
    // FR-OTA-07: the new image boots once, unvalidated. If it cannot establish
    // a broker session the bootloader reverts on the next reset, so nothing
    // here marks it valid.
    ESP_LOGI(TAG, "image written to the inactive slot — restarting to try it");
    vTaskDelay(pdMS_TO_TICKS(500));   // let the log drain before the reset
    esp_restart();
  }

  // FR-OTA-09: a failed or interrupted download leaves the inactive slot
  // unmarked, so nothing bootable was produced. Say so plainly — a silent
  // failure here reads as an update that never started.
  ESP_LOGE(TAG, "download failed (%s) — partial image discarded, still running the old one",
           esp_err_to_name(err));
  indicate(Indication::Operational);
  worker = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool ota_handle_command(const char* payload, size_t len) {
  if (downloading) {
    ESP_LOGW(TAG, "a download is already running — command ignored");
    return false;
  }
  if (len == 0 || len >= sizeof(pending_url)) {
    ESP_LOGW(TAG, "OTA command is %u bytes — not a URL", static_cast<unsigned>(len));
    return false;
  }

  char url[sizeof(pending_url)];
  std::memcpy(url, payload, len);
  url[len] = '\0';

  if (!ota_url_acceptable(url)) {
    // Rejected here, with the value, rather than handed to the downloader. A
    // malformed URL otherwise surfaces as a transport error minutes later with
    // nothing connecting it back to the message that caused it.
    ESP_LOGW(TAG, "not a URL this device will download from: '%s'", url);
    return false;
  }

  std::strcpy(pending_url, url);
  // FR-OTA-08: the download runs in its own task so the meter loop keeps
  // reading. Ingestion stalling for the length of a download would drop several
  // cycles of a household's consumption for an update nobody asked to lose them
  // for.
  if (xTaskCreate(download_task, "ota", 8192, nullptr, 4, &worker) != pdPASS) {
    ESP_LOGE(TAG, "could not start the download task");
    return false;
  }
  return true;
}

void ota_mark_valid_on_session() {
  if (validated) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    validated = true;   // nothing to do: this image was already valid
    return;
  }

  // FR-OTA-05/06. The broker session is the criterion, and the meter is
  // deliberately not: a quiet meter is normal, so requiring a decode would roll
  // back a perfectly healthy build every time the line went silent during the
  // validation window.
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "broker session established — image %s marked valid", app->version);
    validated = true;
  } else {
    ESP_LOGE(TAG, "could not mark the image valid — it will roll back on reset");
  }
}

bool ota_in_progress() { return downloading; }

void ota_start() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  ESP_LOGI(TAG, "running from %s, image state %d", running->label, static_cast<int>(state));
}

}  // namespace gplug
