// Bring-up sketch, not the product.
//
// It exists to prove one chain end to end before any real firmware is written:
// GitHub Actions builds it, the workbench flashes it, and the board shows it is
// alive. It also proves the LED pin assignment in
// docs/Functionality/MBUS-E450-Interface-Spec.md §3, which until now was only
// written down.
//
// The pattern is the startup indication the FSD already specifies (§11.3):
// red -> green -> blue, 500 ms each. If those three LEDs light in that order,
// the pinout and the polarity are both right.

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr gpio_num_t LED_RED = GPIO_NUM_1;
constexpr gpio_num_t LED_BLUE = GPIO_NUM_3;
constexpr gpio_num_t LED_GREEN = GPIO_NUM_4;

// Active high on this board revision. Keeping the polarity behind one constant
// makes a revision that inverts it a one-line change — interface spec §3.
constexpr int LED_ON = 1;
constexpr int LED_OFF = 0;

constexpr TickType_t STEP_MS = 500;

const char *TAG = "gplug";

#if CONFIG_GPLUG_SIM_METER
// The marker the build workflow greps for. It must appear in the simulated
// image and never in the production one; CI fails the build in either
// direction. Checking the artefact rather than the source is what makes the
// check survive a defaults file that stopped being applied.
const char *BUILD_VARIANT = "GPLUG-SIM";
#else
const char *BUILD_VARIANT = "production";
#endif

void configure_leds() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << LED_RED) | (1ULL << LED_BLUE) | (1ULL << LED_GREEN);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    gpio_set_level(LED_RED, LED_OFF);
    gpio_set_level(LED_BLUE, LED_OFF);
    gpio_set_level(LED_GREEN, LED_OFF);
}

void flash(gpio_num_t led, const char *name) {
    gpio_set_level(led, LED_ON);
    ESP_LOGI(TAG, "%s on", name);
    vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    gpio_set_level(led, LED_OFF);
}

}  // namespace

extern "C" void app_main() {
    const esp_app_desc_t *app = esp_app_get_description();

    // Printed once per boot so a serial capture identifies exactly what is
    // running. The version comes from PROJECT_VER, which CI sets from the git
    // tag — an untagged local build shows a commit hash instead.
    ESP_LOGI(TAG, "gPlug-mini %s (%s build), IDF %s", app->version, BUILD_VARIANT,
             app->idf_ver);

    configure_leds();

    while (true) {
        flash(LED_RED, "red");
        flash(LED_GREEN, "green");
        flash(LED_BLUE, "blue");
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}
