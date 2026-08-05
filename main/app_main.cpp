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
#include "config.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "framing.h"
#include "ha_discovery.h"
#include "indicator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "meter_source.h"
#include "net.h"
#include "obis_map.h"
#include "ota.h"
#include "provisioning.h"
#include "sdkconfig.h"

#include "dlms_parser/dlms_parser.h"

extern "C" const char* const GPLUG_BUILD_MARKER;

namespace {

constexpr const char* TAG = "gplug";


// One transmission is a few hundred bytes; a cycle that overruns this is a
// defect worth seeing rather than a buffer worth growing.
constexpr size_t MAX_CYCLE = 2048;

uint32_t now_ms() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// FR-WDT-04 — the reset reason, named, on the console at boot.
//
// Reported for every boot rather than only the interesting ones, because the
// value of the line is in reading it when you did not expect to need it: a
// device that has been quietly resetting for a week looks exactly like one that
// has been up for a week, until this line says otherwise.
void report_reset_reason() {
  const esp_reset_reason_t r = esp_reset_reason();
  const char* name = "unknown";
  switch (r) {
    case ESP_RST_POWERON:  name = "power-on";            break;
    case ESP_RST_EXT:      name = "external reset";      break;
    case ESP_RST_SW:       name = "software restart";    break;
    case ESP_RST_PANIC:    name = "panic";               break;
    case ESP_RST_INT_WDT:  name = "interrupt watchdog";  break;
    case ESP_RST_TASK_WDT: name = "task watchdog";       break;
    case ESP_RST_WDT:      name = "other watchdog";      break;
    case ESP_RST_BROWNOUT: name = "brownout";            break;
    case ESP_RST_DEEPSLEEP:name = "deep sleep wake";     break;
    default: break;
  }
  ESP_LOGI(TAG, "reset reason: %s (%d)", name, static_cast<int>(r));
}

// The same line, once the device is up.
//
// FR-WDT-04 says the reason must be *available* on the console after boot, and
// printing it at 200 ms does not achieve that on this hardware: SLOT1 is a
// native USB JTAG device, so a reset re-enumerates the port and the first ~2.5 s
// of output goes with it. Measured — the earliest line any monitor sees after a
// reset is t=2647 ms, and the reason is long gone by then.
//
// So it is reported a second time after the network is up, which is both
// observable and the more useful moment: a device that has been quietly
// resetting says so on a line that survives.
void report_reset_reason_again() {
  report_reset_reason();
}

// FR-WDT-01/02 — the main loop is subscribed to the task watchdog.
//
// Only this task. It is the one that must keep running for the device to be
// doing its job at all: it reads the meter, assembles cycles and publishes. The
// network tasks are ESP-IDF's and retry on their own; subscribing them would
// turn a broker outage into a reboot, which FR-WDT-05 forbids.
void watchdog_start() {
  // The IDF may already have initialised it from Kconfig. Both outcomes are
  // fine and neither is an error worth aborting for — what matters is that this
  // task ends up subscribed.
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = 5000;                    // FR-WDT-01 names 5 s
  cfg.idle_core_mask = 0;                   // idle tasks are not ours to watch
  cfg.trigger_panic = true;                 // reset, not a warning
  const esp_err_t init = esp_task_wdt_init(&cfg);
  if (init != ESP_OK && init != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "task watchdog init failed (%s) — continuing unwatched",
             esp_err_to_name(init));
    return;
  }
  const esp_err_t add = esp_task_wdt_add(nullptr);
  if (add != ESP_OK) {
    ESP_LOGW(TAG, "could not subscribe to the task watchdog (%s)", esp_err_to_name(add));
    return;
  }
  ESP_LOGI(TAG, "task watchdog: main loop subscribed, %u ms", static_cast<unsigned>(cfg.timeout_ms));
}

size_t values_this_cycle = 0;
unsigned undecoded = 0;

// One cycle's worth of decoded values. Published as a single state message so
// Home Assistant sees one consistent set rather than a dribble of separate
// readings (FR-AGG-03), keeping the first occurrence of each register
// (FR-AGG-04) and staying silent when nothing decoded (FR-AGG-06).
gplug::CycleSet cycle_set;
char meter_serial[32] = "";
// Where the running configuration came from. A bench session that cannot tell
// "provisioned" from "fell back to build defaults" is reading a different code
// path than it thinks (FR-NVS-02).
const char* config_source = "unknown";
bool discovery_done = false;

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

  if (entry->kind == gplug::Kind::CUMULATIVE) {
    ESP_LOGI(TAG, "%-20s = %llu %s (scaler %d)", entry->label,
             static_cast<unsigned long long>(raw), entry->unit,
             c.has_scaler_unit ? c.scaler : 0);
    // Serialised as an integer, never through a float: the value is exact here
    // and must stay exact all the way to the broker (FR-DEC-04).
    if (!cycle_set.add_integer(entry->label, raw)) {
      ESP_LOGD(TAG, "%s already in this cycle — later value discarded (FR-AGG-04)", entry->label);
    }
  } else {
    const double v = static_cast<double>(c.value_as_float_with_scaler_applied());
    ESP_LOGI(TAG, "%-20s = %.3f %s", entry->label, v, entry->unit);
    if (!cycle_set.add_real(entry->label, v)) {
      ESP_LOGD(TAG, "%s already in this cycle — later value discarded (FR-AGG-04)", entry->label);
    }
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

  if (cycle_set.empty()) return;   // FR-AGG-06
  const char* state = cycle_set.json();
  gplug::mqtt_publish_state(state);
  gplug::indicate_publish();   // FR-LED-02: one pulse per set that actually went
  ESP_LOGI(TAG, "published %u value(s), %u B state",
           static_cast<unsigned>(cycle_set.size()), static_cast<unsigned>(std::strlen(state)));
}

}  // namespace

extern "C" void app_main() {
  const esp_app_desc_t* app = esp_app_get_description();
  ESP_LOGI(TAG, "gPlug-mini %s (%s build, %s), IDF %s",
           app->version, GPLUG_BUILD_MARKER, gplug::meter_source_name(), app->idf_ver);

  // FR-WDT-04 — before anything else can overwrite the evidence. A device that
  // reboots in a meter cabinet leaves no other trace of why, and "it came back"
  // reads identically whether it was a power cut or a hung task.
  report_reset_reason();

  // One owner of the LED pins from here on. Two writers make a pattern that is
  // neither, and the symptom is an indicator that looks almost right.
  gplug::indicator_start();   // runs the BOOT sequence, then follows the state
  gplug::indicate(gplug::Indication::Connecting);


  // Storage first, then the configuration, then the network that depends on it.
  // A device that cannot open its storage still runs on build defaults rather
  // than looping — one in a meter cabinet that will not boot needs a visit
  // (FR-NVS-02).
  ESP_ERROR_CHECK(gplug::config_storage_init());
  gplug::Config stored{};
  config_source = (gplug::config_load(stored) == gplug::ConfigFault::None) ? "nvs" : "build";
  const gplug::Config conf = gplug::config_effective();

  // Provisioning only when there is nothing usable to try — neither stored nor
  // built in (FR-NVS-02). A bench build carries Kconfig credentials, so it
  // never lands here; a production build ships with them empty, so it always
  // does on first boot.
  //
  // Never on a WiFi failure. A device in a meter cabinet that answers an outage
  // by raising a portal nobody can see has made itself unreachable rather than
  // recoverable (FR-SUP-04, D-C4).
  if (!gplug::config_usable(conf)) {
    ESP_LOGW(TAG, "no usable configuration (%s) — entering Provisioning",
             gplug::describe(gplug::config_fault(conf)));
    gplug::indicate(gplug::Indication::Provisioning);   // FR-LED-01
    if (gplug::provisioning_run()) {
      ESP_LOGI(TAG, "configuration stored — restarting to use it");
    } else {
      // Nothing was submitted and nothing was stored, so there is still nothing
      // to try. Restarting re-raises the portal rather than proceeding to fail
      // at association forever with credentials the device does not have.
      ESP_LOGW(TAG, "Provisioning timed out with no submission — restarting");
    }
    esp_restart();
  }

  gplug::wifi_start_and_wait(conf);
  gplug::mqtt_start(conf);
  gplug::indicate(gplug::Indication::Operational);   // §11.3: dark between publications
  gplug::ota_start();
  report_reset_reason_again();   // FR-WDT-04, past the USB re-enumeration window

  dlms_parser::DlmsParser parser(on_value, nullptr);
  parser.load_default_patterns();

  gplug::meter_source_start();

  // FR-WDT-01/02, and not one line earlier. Everything above this point can
  // legitimately block for longer than the watchdog period: joining WiFi
  // retries for as long as it takes and a broker session may never arrive, both
  // by design (FR-SUP-04, FR-WDT-05). Subscribing before them makes the
  // watchdog reset a device that is doing exactly what it was told to.
  //
  // Measured: subscribing before wifi_start_and_wait fired the watchdog 5.5 s
  // into every boot.
  watchdog_start();

  std::vector<uint8_t> cycle;
  cycle.reserve(MAX_CYCLE);
  uint32_t last_byte = now_ms();
  uint8_t chunk[256];

  while (true) {
    // FR-WDT-03: fed every pass, and a pass is at most the 200 ms read timeout
    // even when the meter says nothing. A quiet line is normal — the meter is
    // silent between transmissions and can be silent for far longer — so a
    // watchdog fed only when data arrives would reset a healthy device on a
    // quiet meter, which is the failure this requirement forbids.
    esp_task_wdt_reset();

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
    cycle_set.clear();

    // A cycle that opens on a frame boundary starts with the closing flag of
    // the frame before it, and that one byte costs the whole cycle (FR-MTR-06,
    // TS-023). Only leading flags go — the payload behind them still decodes.
    const size_t skip = gplug::leading_flags(cycle.data(), cycle.size());
    if (skip) ESP_LOGD(TAG, "skipped %u leading flag byte(s)", static_cast<unsigned>(skip));

    // Parsed until the buffer stops yielding, not once.
    //
    // One call returns after the first complete APDU and reports how far it
    // got: on the bench that was 110 bytes of a 400-byte telegram, so six
    // registers decoded and everything after them — including the meter
    // identity — was never looked at. Home Assistant discovery waits on that
    // identity (FR-HA-03), so the device connected, decoded, and published
    // nothing at all.
    //
    // A pass consuming zero bytes ends it, so a buffer the parser cannot
    // advance through terminates rather than spinning.
    size_t offset = skip, objects = 0, consumed = 0, passes = 0;
    while (offset < cycle.size()) {
      const auto pass = parser.parse({ cycle.data() + offset, cycle.size() - offset });
      objects += pass.count;
      consumed += pass.bytes_consumed;
      ++passes;
      if (pass.bytes_consumed == 0) break;
      offset += pass.bytes_consumed;
    }
    struct { size_t count, bytes_consumed; } result{ objects, consumed };
    // Stack headroom after the parse, not before: the parser recurses over the
    // AXDR structure, so the depth depends on the telegram and the worst case
    // is whatever the meter sends, not whatever we tested with.
    // One line per cycle carrying everything a bench session asks about, so a
    // question costs a read rather than a reflash: what the device thinks its
    // state is, where its configuration came from, whether it knows the meter,
    // and whether the broker is there. Cheap, always on, and it is the line
    // every bench test greps.
    ESP_LOGI(TAG, "diag: state=%s cfg=%s meter=%s broker=%s values=%u",
             gplug::indication_name(),
             config_source,
             meter_serial[0] ? meter_serial : "-",
             gplug::mqtt_connected() ? "up" : "down",
             static_cast<unsigned>(values_this_cycle));

    ESP_LOGI(TAG, "cycle: %u bytes, %u objects, %u consumed in %u pass(es), %u B stack free",
             static_cast<unsigned>(cycle.size()),
             static_cast<unsigned>(result.count),
             static_cast<unsigned>(result.bytes_consumed),
             static_cast<unsigned>(passes),
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
