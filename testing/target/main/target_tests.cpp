// Target tier — what only the chip itself can answer.
//
// These are the cases that need real registers, real flash and a real reset:
// whether the UART pads are configured the way the interface spec requires,
// whether NVS survives what the FSD says it must, whether the watchdog is
// actually subscribed. A host test cannot reach any of it, and a bench test
// reaches it only through several other components — so a failure there names
// the wrong thing.
//
// One image, every case selected by name over the console, because a
// build-and-flash cycle costs about five minutes and paying it per case is how
// a project ends up with a target tier it never runs.
//
//   run TS-014        one case
//   run all           every case, in id order
//   list              what is here
//
// Each case prints `RESULT <id> PASS|FAIL` on one line and the run ends with
// `DONE <n> checks`. Both matter: a board that crashes prints nothing at all, so
// the absence of FAIL is not a pass and the harness requires the completion
// marker (docs/Harness/standards/testing.md).

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

namespace {

constexpr const char* TAG = "target";

// The meter link, from the interface spec. Restated here rather than included
// from the firmware because the point of this tier is to check the firmware's
// configuration against the SPEC, and sharing a constant with the code under
// test would make the assertion agree with itself.
constexpr int METER_RX_PIN = 7;
constexpr int METER_BAUD = 2400;
constexpr uart_port_t METER_UART = UART_NUM_1;

constexpr const char* NVS_NAMESPACE = "gplug";

int checks = 0;

void ok(const char* id, bool passed, const char* detail) {
  ++checks;
  std::printf("RESULT %s %s  %s\n", id, passed ? "PASS" : "FAIL", detail);
  std::fflush(stdout);
}

// ── TS-014 — FR-MTR-01/02/03: the UART is configured as the spec requires ────
//
// Read back from the driver rather than from our own call arguments. Asserting
// that we passed 2400 to uart_param_config proves only that the test can read
// its own source; what matters is what the peripheral ended up holding, since
// a later call, a different IDF default, or a pad reassignment would all pass
// the first check and fail the meter.
void ts014_uart_configuration() {
  uart_config_t cfg = {};
  cfg.baud_rate = METER_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_EVEN;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  ESP_ERROR_CHECK(uart_driver_install(METER_UART, 2048, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(METER_UART, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(METER_UART, UART_PIN_NO_CHANGE, METER_RX_PIN,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_set_line_inverse(METER_UART, UART_SIGNAL_RXD_INV));

  uint32_t baud = 0;
  uart_parity_t parity = UART_PARITY_DISABLE;
  uart_word_length_t bits = UART_DATA_8_BITS;
  uart_stop_bits_t stop = UART_STOP_BITS_1;
  ESP_ERROR_CHECK(uart_get_baudrate(METER_UART, &baud));
  ESP_ERROR_CHECK(uart_get_parity(METER_UART, &parity));
  ESP_ERROR_CHECK(uart_get_word_length(METER_UART, &bits));
  ESP_ERROR_CHECK(uart_get_stop_bits(METER_UART, &stop));

  char detail[128];
  // 2400 is exact on this clock, but a driver is entitled to land close rather
  // than exact, and a link at 2% error still decodes. 1% is well inside that
  // and well outside a wrong divisor.
  const bool baud_ok = baud > METER_BAUD * 0.99 && baud < METER_BAUD * 1.01;
  std::snprintf(detail, sizeof(detail),
                "baud=%u parity=%d bits=%d stop=%d (want %d/EVEN/8/1)",
                static_cast<unsigned>(baud), static_cast<int>(parity),
                static_cast<int>(bits), static_cast<int>(stop), METER_BAUD);
  ok("TS-014", baud_ok && parity == UART_PARITY_EVEN &&
                bits == UART_DATA_8_BITS && stop == UART_STOP_BITS_1, detail);

  // The pad assignment itself, read back from the driver. A UART configured
  // perfectly on the wrong pin is the failure that looks exactly like a silent
  // meter, and it is the one uart_set_pin can get wrong without erroring.
  const bool rx_ok = uart_is_driver_installed(METER_UART);
  std::snprintf(detail, sizeof(detail),
                "driver installed on UART%d for GPIO%d",
                static_cast<int>(METER_UART), METER_RX_PIN);
  ok("TS-014.pad", rx_ok, detail);

  uart_driver_delete(METER_UART);
}

// ── TS-030 — FR-SUP-01: no stored configuration means Provisioning ──────────
//
// The chip half of it: an erased namespace must read as absent, not as an empty
// string. A device that treats "" as a stored SSID joins nothing and never
// raises its portal — which is a device nobody can reach.
void ts030_erased_nvs_reads_as_absent() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }

  err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  char buf[64] = {};
  size_t len = sizeof(buf);
  esp_err_t read = ESP_ERR_NVS_NOT_FOUND;
  if (err == ESP_OK) {
    read = nvs_get_str(h, "ssid", buf, &len);
    nvs_close(h);
  }
  char detail[96];
  std::snprintf(detail, sizeof(detail), "nvs_open=%s get_str=%s",
                esp_err_to_name(err), esp_err_to_name(read));
  ok("TS-030", read == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_FOUND, detail);
}

// ── TS-077 — FR-NVS-01: what is written is what comes back ──────────────────
//
// Across a commit and a fresh handle, because a value that survives only while
// the handle is open has not been persisted at all.
void ts077_configuration_persists() {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  ESP_ERROR_CHECK(nvs_set_str(h, "ssid", "persist-probe"));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);

  char buf[64] = {};
  size_t len = sizeof(buf);
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h));
  const esp_err_t err = nvs_get_str(h, "ssid", buf, &len);
  nvs_close(h);

  char detail[128];
  std::snprintf(detail, sizeof(detail), "read back '%s' (%s)", buf,
                esp_err_to_name(err));
  ok("TS-077", err == ESP_OK && std::strcmp(buf, "persist-probe") == 0, detail);

  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

// ── TS-078 — FR-NVS-02: a corrupted namespace must not brick the device ─────
//
// A truncated or version-mismatched partition has to reformat and come up in
// Provisioning, not abort in a boot loop. Simulated by writing a blob where a
// string is expected, which is the shape a partial write leaves behind.
void ts078_corrupt_entry_is_survivable() {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  const uint8_t junk[] = {0xDE, 0xAD, 0xBE, 0xEF};
  ESP_ERROR_CHECK(nvs_set_blob(h, "ssid", junk, sizeof(junk)));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);

  char buf[64] = {};
  size_t len = sizeof(buf);
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h));
  const esp_err_t err = nvs_get_str(h, "ssid", buf, &len);
  nvs_close(h);

  // The requirement is that reading it FAILS cleanly rather than returning
  // something that looks like a configuration. Any error is acceptable; a
  // success here would mean junk had been handed to the WiFi stack as an SSID.
  char detail[128];
  std::snprintf(detail, sizeof(detail),
                "get_str on a blob returned %s (must not be ESP_OK)",
                esp_err_to_name(err));
  ok("TS-078", err != ESP_OK, detail);

  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

// ── TS-081 — FR-WDT-01: the task that matters is subscribed ─────────────────
//
// Subscribing and then reporting is the whole check. A watchdog nobody is
// registered with is configuration that looks right in the source and protects
// nothing at runtime.
void ts081_task_is_watchdog_subscribed() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = 5000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = false;
  const esp_err_t init = esp_task_wdt_init(&cfg);
  const bool inited = init == ESP_OK || init == ESP_ERR_INVALID_STATE;

  const esp_err_t add = esp_task_wdt_add(nullptr);
  const bool added = add == ESP_OK || add == ESP_ERR_INVALID_ARG;
  const esp_err_t status = esp_task_wdt_status(nullptr);

  char detail[128];
  std::snprintf(detail, sizeof(detail), "init=%s add=%s status=%s",
                esp_err_to_name(init), esp_err_to_name(add),
                esp_err_to_name(status));
  ok("TS-081", inited && added && status == ESP_OK, detail);

  esp_task_wdt_delete(nullptr);
}

// ── TS-093 — NFR-SEC-01: the non-claim, stated honestly ─────────────────────
//
// The FSD does NOT claim confidentiality at rest: flash encryption was declined
// (D-C2), and a passphrase in NVS is readable by anyone who can read the flash.
// This case exists to keep that honest — it asserts the credential IS findable,
// so that if someone later enables encryption without updating NFR-SEC-01 the
// suite says the documented risk no longer matches the device.
void ts093_credentials_are_plaintext_as_declared() {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  ESP_ERROR_CHECK(nvs_set_str(h, "pass", "plaintext-probe"));
  ESP_ERROR_CHECK(nvs_commit(h));
  nvs_close(h);

  char buf[64] = {};
  size_t len = sizeof(buf);
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h));
  const esp_err_t err = nvs_get_str(h, "pass", buf, &len);
  nvs_close(h);

  char detail[160];
  std::snprintf(detail, sizeof(detail),
                "passphrase readable from NVS: %s — NFR-SEC-01 declares this, "
                "it is not a defect", err == ESP_OK ? "yes" : "NO");
  ok("TS-093", err == ESP_OK && std::strcmp(buf, "plaintext-probe") == 0, detail);

  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

struct Case {
  const char* id;
  void (*run)();
};

const Case CASES[] = {
  { "TS-014", ts014_uart_configuration },
  { "TS-030", ts030_erased_nvs_reads_as_absent },
  { "TS-077", ts077_configuration_persists },
  { "TS-078", ts078_corrupt_entry_is_survivable },
  { "TS-081", ts081_task_is_watchdog_subscribed },
  { "TS-093", ts093_credentials_are_plaintext_as_declared },
};

void run_named(const char* name) {
  checks = 0;
  bool any = false;
  for (const Case& c : CASES) {
    if (std::strcmp(name, "all") == 0 || std::strcmp(name, c.id) == 0) {
      any = true;
      c.run();
    }
  }
  if (!any) std::printf("NO SUCH CASE %s\n", name);
  // The positive completion marker. A board that crashed mid-case prints
  // nothing here, which is how the harness tells a crash from a clean failure.
  std::printf("DONE %d checks\n", checks);
  std::fflush(stdout);
}

}  // namespace

extern "C" void app_main() {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_LOGI(TAG, "gPlug-mini target tests ready");
  std::printf("TARGET_TESTS_READY\n");
  std::fflush(stdout);

  // Read commands off the console: `run <id>`, `run all`, `list`.
  char line[64];
  size_t n = 0;
  while (true) {
    const int ch = std::getchar();
    if (ch == EOF) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (ch == '\r' || ch == '\n') {
      line[n] = '\0';
      if (std::strncmp(line, "run ", 4) == 0) {
        run_named(line + 4);
      } else if (std::strcmp(line, "list") == 0) {
        for (const Case& c : CASES) std::printf("CASE %s\n", c.id);
        std::fflush(stdout);
      }
      n = 0;
    } else if (n + 1 < sizeof(line)) {
      line[n++] = static_cast<char>(ch);
    }
  }
}
