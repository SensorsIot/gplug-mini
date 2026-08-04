// Simulated meter source — compiled only into the simulated build.
//
// FR-BLD-04 requires the production binary to be incapable of fabricating
// readings, and the check is performed on the artefact: the capture bytes must
// not be findable in it. The #if guard is what makes that true, and CI verifies
// the outcome rather than trusting the guard.
#include "sdkconfig.h"

#if CONFIG_GPLUG_SIM_METER

#include <cstring>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "meter_source.h"

// The marker CI greps for. It must appear in this binary and never in the
// production one; the workflow fails the build in either direction.
extern "C" const char* const GPLUG_BUILD_MARKER = "GPLUG-SIM";

// The same capture the host tests run against — one file, so a change to the
// fixture changes both tiers together. Embedded as text and parsed at boot
// rather than committed twice in two encodings.
extern const uint8_t capture_start[] asm("_binary_e450_serial8_hex_start");
extern const uint8_t capture_end[] asm("_binary_e450_serial8_hex_end");

namespace gplug {
namespace {

constexpr const char* TAG = "sim";

// The meter pushes on a cycle and is silent between them. Replaying without
// that silence would let a decoder pass here and fail on the real meter, since
// the gap is what ends a cycle (FR-AGG-01).
constexpr uint32_t CYCLE_GAP_MS = 10000;

std::vector<uint8_t> capture;
size_t pos = 0;
bool gap_pending = false;

void parse_hex() {
  const char* p = reinterpret_cast<const char*>(capture_start);
  const char* end = reinterpret_cast<const char*>(capture_end);
  uint8_t byte = 0;
  int nibbles = 0;
  for (; p < end; ++p) {
    int v;
    if (*p >= '0' && *p <= '9') v = *p - '0';
    else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
    else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
    else continue;                       // whitespace and line breaks
    byte = static_cast<uint8_t>((byte << 4) | v);
    if (++nibbles == 2) { capture.push_back(byte); byte = 0; nibbles = 0; }
  }
}

}  // namespace

void meter_source_start() {
  parse_hex();
  ESP_LOGI(TAG, "simulated meter: %u byte capture, %u ms between cycles",
           static_cast<unsigned>(capture.size()), static_cast<unsigned>(CYCLE_GAP_MS));
  if (capture.empty()) ESP_LOGE(TAG, "capture is empty — check the embedded fixture");
}

size_t meter_source_read(uint8_t* out, size_t max, uint32_t timeout_ms) {
  if (capture.empty()) { vTaskDelay(pdMS_TO_TICKS(timeout_ms)); return 0; }

  // Between cycles, be silent for real — the aggregator's boundary detection is
  // the thing under test, and it can only work against actual silence.
  if (gap_pending) {
    vTaskDelay(pdMS_TO_TICKS(CYCLE_GAP_MS));
    gap_pending = false;
    pos = 0;
    return 0;
  }

  const size_t n = std::min(max, capture.size() - pos);
  std::memcpy(out, capture.data() + pos, n);
  pos += n;
  if (pos >= capture.size()) gap_pending = true;
  return n;
}

const char* meter_source_name() { return "simulated"; }

}  // namespace gplug

#endif  // CONFIG_GPLUG_SIM_METER
