// gPlug-mini — reads a Landis+Gyr E450 customer interface and decodes it.
//
// This increment proves the decode path on target: source seam, cycle-boundary
// detection, dlms_parser, OBIS mapping. Publishing to MQTT comes next; until
// then decoded values go to the console, which is what the bench reads.
//
// The startup indication is red -> green -> blue, 500 ms each (FSD §11.3).

#include <cstdint>
#include <cstring>
#include <vector>

#include "aggregator.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "meter_source.h"
#include "obis_map.h"
#include "sdkconfig.h"

#include "dlms_parser/dlms_parser.h"

extern "C" const char* const GPLUG_BUILD_MARKER;

namespace {

constexpr const char* TAG = "gplug";

constexpr gpio_num_t LED_RED = GPIO_NUM_1;
constexpr gpio_num_t LED_BLUE = GPIO_NUM_3;
constexpr gpio_num_t LED_GREEN = GPIO_NUM_4;
constexpr int LED_ON = 1;   // active high on this board revision
constexpr int LED_OFF = 0;

// One transmission is a few hundred bytes; a cycle that overruns this is a
// defect worth seeing rather than a buffer worth growing.
constexpr size_t MAX_CYCLE = 2048;

uint32_t now_ms() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void configure_leds() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = (1ULL << LED_RED) | (1ULL << LED_BLUE) | (1ULL << LED_GREEN);
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  for (auto led : { LED_RED, LED_GREEN, LED_BLUE }) gpio_set_level(led, LED_OFF);
}

void startup_indication() {
  for (auto led : { LED_RED, LED_GREEN, LED_BLUE }) {
    gpio_set_level(led, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(led, LED_OFF);
  }
}

size_t values_this_cycle = 0;

void on_value(const dlms_parser::AxdrCapture& c) {
  std::array<char, 32> obis_buf;
  const std::string_view obis_view = c.obis.to_string(obis_buf);
  char obis[40];
  const size_t n = std::min(obis_view.size(), sizeof(obis) - 1);
  std::memcpy(obis, obis_view.data(), n);
  obis[n] = '\0';

  const gplug::ObisEntry* entry = gplug::obis_lookup(obis);
  ++values_this_cycle;

  if (entry == nullptr) {
    // Not an error: the register set is a deployment choice. Logged so an
    // unmapped code is discoverable rather than silently dropped.
    ESP_LOGD(TAG, "unmapped OBIS %s", obis);
    return;
  }

  if (!c.is_numeric()) {
    std::array<char, 128> buf;
    const std::string_view s = c.value_as_string(buf);
    ESP_LOGI(TAG, "%-20s = %.*s", entry->label, static_cast<int>(s.size()), s.data());
    return;
  }

  // Cumulative registers stay integers all the way through: float is exact only
  // to 16,777,216 and a lifetime total in Wh passes that at 16.8 MWh, after
  // which a total quantises and invents consumption that never happened
  // (FR-DEC-04).
  uint64_t raw = 0;
  for (auto b : c.value) raw = (raw << 8) | b;

  if (entry->kind == gplug::Kind::CUMULATIVE) {
    ESP_LOGI(TAG, "%-20s = %llu %s (scaler %d)", entry->label,
             static_cast<unsigned long long>(raw), entry->unit,
             c.has_scaler_unit ? c.scaler : 0);
  } else {
    ESP_LOGI(TAG, "%-20s = %.3f %s", entry->label,
             static_cast<double>(c.value_as_float_with_scaler_applied()), entry->unit);
  }
}

}  // namespace

extern "C" void app_main() {
  const esp_app_desc_t* app = esp_app_get_description();
  ESP_LOGI(TAG, "gPlug-mini %s (%s build, %s), IDF %s",
           app->version, GPLUG_BUILD_MARKER, gplug::meter_source_name(), app->idf_ver);

  configure_leds();
  startup_indication();

  dlms_parser::DlmsParser parser(on_value, nullptr);
  parser.load_default_patterns();

  gplug::meter_source_start();

  std::vector<uint8_t> cycle;
  cycle.reserve(MAX_CYCLE);
  uint32_t last_byte = now_ms();
  uint8_t chunk[256];

  while (true) {
    const size_t n = gplug::meter_source_read(chunk, sizeof(chunk), 200);
    if (n > 0) {
      if (cycle.size() + n > MAX_CYCLE) {
        ESP_LOGW(TAG, "cycle exceeded %u bytes — discarding", static_cast<unsigned>(MAX_CYCLE));
        cycle.clear();
      }
      cycle.insert(cycle.end(), chunk, chunk + n);
      last_byte = now_ms();
      continue;
    }

    if (!gplug::cycle_ended(now_ms(), last_byte, cycle.size())) continue;

    values_this_cycle = 0;
    const auto result = parser.parse({ cycle.data(), cycle.size() });
    // Stack headroom after the parse, not before: the parser recurses over the
    // AXDR structure, so the depth depends on the telegram and the worst case
    // is whatever the meter sends, not whatever we tested with.
    ESP_LOGI(TAG, "cycle: %u bytes, %u objects, %u consumed, %u B stack free",
             static_cast<unsigned>(cycle.size()),
             static_cast<unsigned>(result.count),
             static_cast<unsigned>(result.bytes_consumed),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    if (result.count == 0) {
      // A burst arrived and nothing decoded. Distinct from silence, and worth
      // its own line — this is the signature of a wrong serial length or a
      // register set nobody mapped (FR-ERR-03).
      ESP_LOGW(TAG, "burst received but nothing decoded");
    }
    cycle.clear();
  }
}
