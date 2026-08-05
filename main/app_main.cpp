// gPlug-mini — reads a Landis+Gyr E450 customer interface and decodes it.
//
// This increment proves the decode path on target: source seam, cycle-boundary
// detection, dlms_parser, OBIS mapping. Publishing to MQTT comes next; until
// then decoded values go to the console, which is what the bench reads.
//
// The startup indication is red -> green -> blue, 500 ms each (FSD §11.3).

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include "aggregator.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ha_discovery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "meter_source.h"
#include "net.h"
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
unsigned undecoded = 0;

// One cycle's worth of decoded values, assembled as JSON. Published as a single
// state message so Home Assistant sees one consistent set rather than a dribble
// of separate readings.
char state_json[768];
size_t state_len = 0;
char meter_serial[32] = "";
bool discovery_done = false;

void json_append(const char* fmt, ...) {
  if (state_len + 2 >= sizeof(state_json)) return;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(state_json + state_len, sizeof(state_json) - state_len, fmt, args);
  va_end(args);
  if (n > 0 && state_len + static_cast<size_t>(n) < sizeof(state_json)) state_len += n;
}

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
    // The serial keys every entity's identity, so it is kept rather than only
    // logged. Discovery waits for it (FR-HA-03).
    if (entry->kind == gplug::Kind::IDENTITY && meter_serial[0] == '\0' && !s.empty()) {
      const size_t k = std::min(s.size(), sizeof(meter_serial) - 1);
      std::memcpy(meter_serial, s.data(), k);
      meter_serial[k] = '\0';
    }
    return;
  }

  // Cumulative registers stay integers all the way through: float is exact only
  // to 16,777,216 and a lifetime total in Wh passes that at 16.8 MWh, after
  // which a total quantises and invents consumption that never happened
  // (FR-DEC-04).
  uint64_t raw = 0;
  for (auto b : c.value) raw = (raw << 8) | b;

  json_append("%s\"%s\":", state_len ? "," : "{", entry->label);

  if (entry->kind == gplug::Kind::CUMULATIVE) {
    ESP_LOGI(TAG, "%-20s = %llu %s (scaler %d)", entry->label,
             static_cast<unsigned long long>(raw), entry->unit,
             c.has_scaler_unit ? c.scaler : 0);
    // Serialised as an integer, never through a float: the value is exact here
    // and must stay exact all the way to the broker (FR-DEC-04).
    json_append("%llu", static_cast<unsigned long long>(raw));
  } else {
    const double v = static_cast<double>(c.value_as_float_with_scaler_applied());
    ESP_LOGI(TAG, "%-20s = %.3f %s", entry->label, v, entry->unit);
    json_append("%.3f", v);
  }
}

// Discovery once the serial is known, then the cycle's values as one message.
void publish_cycle() {
  if (!gplug::mqtt_connected()) {
    ESP_LOGW(TAG, "broker not connected — cycle dropped, not queued");
    return;   // FR-AGG-05: a set that cannot be delivered now is stale later
  }
  if (meter_serial[0] == '\0') {
    ESP_LOGI(TAG, "no meter serial yet — deferring discovery (FR-HA-03)");
    return;
  }

  if (!discovery_done) {
    char topic[128], payload[768];
    size_t n = 0;
    for (const char* obis : { "1.1.1.8.0.255", "1.1.2.8.0.255", "1.1.5.8.0.255",
                              "1.1.6.8.0.255", "1.1.7.8.0.255", "1.1.8.8.0.255",
                              "1.0.1.7.0.255", "1.0.2.7.0.255" }) {
      const gplug::ObisEntry* e = gplug::obis_lookup(obis);
      if (e == nullptr) continue;
      if (gplug::discovery_topic(topic, sizeof(topic), meter_serial, e->label) == 0) continue;
      if (gplug::discovery_payload(payload, sizeof(payload), *e, meter_serial,
                                   gplug::wifi_mac()) == 0) continue;
      gplug::mqtt_publish_discovery(topic, payload);
      ++n;
    }
    discovery_done = true;
    ESP_LOGI(TAG, "published %u discovery configs for meter %s",
             static_cast<unsigned>(n), meter_serial);
  }

  if (state_len == 0) return;
  json_append("}");
  gplug::mqtt_publish_state(state_json);
  ESP_LOGI(TAG, "published %u B state", static_cast<unsigned>(state_len));
}

}  // namespace

extern "C" void app_main() {
  const esp_app_desc_t* app = esp_app_get_description();
  ESP_LOGI(TAG, "gPlug-mini %s (%s build, %s), IDF %s",
           app->version, GPLUG_BUILD_MARKER, gplug::meter_source_name(), app->idf_ver);

  configure_leds();
  startup_indication();

  gplug::wifi_start_and_wait();
  gplug::mqtt_start();

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
    state_len = 0;
    state_json[0] = '\0';
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
      // register set nobody mapped (FR-MTR-14).
      //
      // The head of the buffer comes with it. "Nothing decoded" alone cannot
      // distinguish a framing error from an unencrypted telegram we mis-parse
      // from an encrypted one, and those want completely different fixes.
      // 7E marks HDLC; 68 marks an M-Bus long frame; noise looks like neither.
      ESP_LOGW(TAG, "burst received but nothing decoded (FR-MTR-14)");
      char head[3 * 32 + 1];
      const size_t shown = std::min<size_t>(cycle.size(), 32);
      for (size_t i = 0; i < shown; ++i) {
        std::snprintf(head + i * 3, 4, "%02X ", cycle[i]);
      }
      head[shown * 3] = '\0';
      ESP_LOGW(TAG, "  first %u bytes: %s", static_cast<unsigned>(shown), head);

      // Whether an HDLC flag appears anywhere is the question that splits the
      // two causes apart. No 0x7E at all means the line settings are wrong and
      // these are not bytes the meter sent. A 0x7E part-way in means the
      // settings are right and reception simply began mid-burst, which is
      // ordinary and FR-MTR-06's job to recover from.
      // A lone 0x7E proves nothing — in 319 bytes of noise one turns up by
      // chance. What marks real framing is the pair 7E A0: the flag followed
      // by the HDLC format byte these meters use. Look for that.
      size_t flags = 0, first_flag = cycle.size();
      bool framed = false;
      for (size_t i = 0; i < cycle.size(); ++i) {
        if (cycle[i] != 0x7E) continue;
        if (!flags) first_flag = i;
        ++flags;
        if (i + 1 < cycle.size() && (cycle[i + 1] & 0xF0) == 0xA0) framed = true;
      }
      ESP_LOGW(TAG, "  0x7E flags: %u, first at offset %u of %u, 7E-A0 pair: %s",
               static_cast<unsigned>(flags), static_cast<unsigned>(first_flag),
               static_cast<unsigned>(cycle.size()), framed ? "yes" : "no");

      if (!framed) {
        // Only now is the line worth doubting. Two in a row, because one
        // corrupt burst is ordinary.
        if (++undecoded >= 2) {
          undecoded = 0;
          gplug::meter_source_try_next_line();
        }
      } else {
        // Framing is right; reception simply began mid-burst, or a frame was
        // corrupt. Not a reason to touch the line settings.
        undecoded = 0;
        char frame[3 * 24 + 1];
        const size_t k = std::min<size_t>(cycle.size() - first_flag, 24);
        for (size_t i = 0; i < k; ++i) {
          std::snprintf(frame + i * 3, 4, "%02X ", cycle[first_flag + i]);
        }
        frame[k * 3] = '\0';
        ESP_LOGW(TAG, "  from first flag: %s", frame);
      }
    } else {
      undecoded = 0;
    }

    publish_cycle();
    cycle.clear();
  }
}
