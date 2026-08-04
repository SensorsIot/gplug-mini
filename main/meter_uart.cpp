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

void meter_source_start() {
  uart_config_t cfg = {};
  cfg.baud_rate = BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_EVEN;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  ESP_ERROR_CHECK(uart_driver_install(PORT, RX_BUFFER, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(PORT, UART_PIN_NO_CHANGE, RX_PIN,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  // The interface presents an inverted signal. Without this every byte framing
  // fails and the port looks dead rather than misconfigured.
  ESP_ERROR_CHECK(uart_set_line_inverse(PORT, UART_SIGNAL_RXD_INV));

  ESP_LOGI(TAG, "meter UART on GPIO%d, %d 8E1, RX inverted, listen-only",
           RX_PIN, BAUD);
}

size_t meter_source_read(uint8_t* out, size_t max, uint32_t timeout_ms) {
  const int n = uart_read_bytes(PORT, out, max, pdMS_TO_TICKS(timeout_ms));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

const char* meter_source_name() { return "meter UART"; }

}  // namespace gplug

#endif  // !CONFIG_GPLUG_SIM_METER
