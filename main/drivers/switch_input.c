#include "drivers/switch_input.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include "common/app_log.h"

static const char *TAG = "switch_input";

static int s_last_level;
static int64_t s_last_change_us;
static uint32_t s_pending_toggles;

esp_err_t switch_input_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << SWITCH_INPUT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        APP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_last_level = gpio_get_level(SWITCH_INPUT_GPIO);
    s_last_change_us = esp_timer_get_time();
    APP_LOGI(TAG, "switch ready: gpio=%d level=%d (low active)", SWITCH_INPUT_GPIO, s_last_level);
    return ESP_OK;
}

uint32_t switch_input_take_toggles(void)
{
    const int level = gpio_get_level(SWITCH_INPUT_GPIO);
    const int64_t now_us = esp_timer_get_time();
    if (level != s_last_level &&
        now_us - s_last_change_us >= (int64_t)SWITCH_INPUT_DEBOUNCE_MS * 1000) {
        s_last_change_us = now_us;
        s_last_level = level;
        s_pending_toggles++;
        APP_LOGI(TAG, "toggle: level=%d pending=%u", level, (unsigned)s_pending_toggles);
    }
    const uint32_t toggles = s_pending_toggles;
    s_pending_toggles = 0;
    return toggles;
}
