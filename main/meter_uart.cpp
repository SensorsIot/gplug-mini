// Production meter source — the real customer interface.
//
// 2400 8E1 with the receive signal inverted, on GPIO7 (interface spec §2.2).
// The device never transmits: the interface belongs to a sealed metering device
// (FR-MTR-04), so no TX pin is configured at all rather than configured and left
// unused.
#include "sdkconfig.h"

#if !CONFIG_GPLUG_SIM_METER

#include "driver/uart.h"
#include "esp_log.h"
#include "meter_source.h"

extern "C" const char* const GPLUG_BUILD_MARKER = "production";

namespace gplug {
namespace {

constexpr const char* TAG = "meter";
constexpr uart_port_t PORT = UART_NUM_1;
constexpr int RX_PIN = 7;
constexpr int BAUD = 2400;

// One cycle is a few hundred bytes; at 2400 baud they arrive over seconds, so
// the buffer only has to outlast the read interval, not a whole cycle.
constexpr size_t RX_BUFFER = 1024;

}  // namespace

namespace {

// The interface spec says the meter presents an inverted signal, and a real
// meter does. A bench source may not: an M-Bus simulator can drive either
// polarity, and either parity, and the wrong choice yields bytes rather than
// silence — plausible-looking rubbish that no decoder can use.
//
// So the line settings are probed rather than asserted. Each candidate gets a
// few cycles; if nothing decodes, the next one is tried. The order starts with
// what the specification says, so a correct installation locks on immediately
// and the probe costs nothing.
struct LineConfig {
  uart_parity_t parity;
  bool invert;
  const char* name;
};

constexpr LineConfig CANDIDATES[] = {
    { UART_PARITY_EVEN,     true,  "8E1 inverted (interface spec)" },
    { UART_PARITY_EVEN,     false, "8E1 normal" },
    { UART_PARITY_DISABLE,  true,  "8N1 inverted" },
    { UART_PARITY_DISABLE,  false, "8N1 normal" },
};

size_t candidate = 0;
bool driver_installed = false;

void apply(const LineConfig& c) {
  uart_config_t cfg = {};
  cfg.baud_rate = BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = c.parity;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (!driver_installed) {
    ESP_ERROR_CHECK(uart_driver_install(PORT, RX_BUFFER, 0, 0, nullptr, 0));
    driver_installed = true;
  }
  ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(PORT, UART_PIN_NO_CHANGE, RX_PIN,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_set_line_inverse(PORT, c.invert ? UART_SIGNAL_RXD_INV
                                                       : UART_SIGNAL_INV_DISABLE));
  uart_flush_input(PORT);
  ESP_LOGI(TAG, "meter UART on GPIO%d, %d %s, listen-only", RX_PIN, BAUD, c.name);
}

}  // namespace

void meter_source_start() {
  apply(CANDIDATES[candidate]);
}

// Called when a burst decoded to nothing. Returns true if the line settings
// changed, so the caller can say so.
bool meter_source_try_next_line() {
  candidate = (candidate + 1) % (sizeof(CANDIDATES) / sizeof(CANDIDATES[0]));
  ESP_LOGW(TAG, "nothing decodes — trying %s", CANDIDATES[candidate].name);
  apply(CANDIDATES[candidate]);
  return true;
}

size_t meter_source_read(uint8_t* out, size_t max, uint32_t timeout_ms) {
  const int n = uart_read_bytes(PORT, out, max, pdMS_TO_TICKS(timeout_ms));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

const char* meter_source_name() { return "meter UART"; }

}  // namespace gplug

#endif  // !CONFIG_GPLUG_SIM_METER
