// The indicator task — the only writer of the LED pins (FR-LED-01..05).
#include "indicator.h"

#include <initializer_list>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace gplug {
namespace {

constexpr const char* TAG = "led";

constexpr gpio_num_t RED = GPIO_NUM_1;
constexpr gpio_num_t BLUE = GPIO_NUM_3;
constexpr gpio_num_t GREEN = GPIO_NUM_4;
constexpr int ON = 1;    // active high on this board revision
constexpr int OFF = 0;

// Fine enough to render the shortest pattern §11.3 asks for: Updating
// alternates every 100 ms, so a tick of 25 ms puts four samples in each phase.
// A coarser tick would round the alternation into something slower.
constexpr uint32_t TICK_MS = 25;
constexpr uint32_t BOOT_STEP_MS = 500;

volatile Indication current = Indication::Boot;
volatile int64_t pulse_until_us = 0;
TaskHandle_t task_handle = nullptr;

void set(bool r, bool g, bool b) {
  gpio_set_level(RED, r ? ON : OFF);
  gpio_set_level(GREEN, g ? ON : OFF);
  gpio_set_level(BLUE, b ? ON : OFF);
}

void boot_sequence() {
  // §11.3: red, then green, then blue, 500 ms each. It is also the only proof
  // that all three colours work, which matters on a board where a dark
  // indicator is otherwise a valid state.
  for (auto led : { RED, GREEN, BLUE }) {
    set(led == RED, led == GREEN, led == BLUE);
    vTaskDelay(pdMS_TO_TICKS(BOOT_STEP_MS));
  }
  set(false, false, false);
}

void indicator_task(void*) {
  boot_sequence();
  uint32_t elapsed = 0;
  while (true) {
    const Pattern p = pattern_for(current);

    if (esp_timer_get_time() < pulse_until_us) {
      set(false, true, false);                    // FR-LED-02
    } else if (p.period_ms == 0) {
      set(p.a_red, p.a_green, p.a_blue);
    } else {
      const bool phase_a = (elapsed % p.period_ms) < (p.period_ms / 2);
      if (phase_a) {
        set(p.a_red, p.a_green, p.a_blue);
      } else {
        set(p.b_red, p.b_green, p.b_blue);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    elapsed += TICK_MS;
  }
}

}  // namespace

void indicator_start() {
  if (task_handle) return;

  gpio_config_t cfg = {};
  cfg.pin_bit_mask = (1ULL << RED) | (1ULL << BLUE) | (1ULL << GREEN);
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  set(false, false, false);

  // Low priority on purpose. An indicator that preempts the meter reader trades
  // a reading for a blink, and the reading is the product.
  xTaskCreate(indicator_task, "led", 2048, nullptr, 2, &task_handle);
  ESP_LOGI(TAG, "indicator started");
}

void indicate(Indication i) { current = i; }

void indicate_publish() {
  pulse_until_us = esp_timer_get_time() + static_cast<int64_t>(PUBLISH_PULSE_MS) * 1000;
}

}  // namespace gplug
