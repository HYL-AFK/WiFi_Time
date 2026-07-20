#include "app_health.h"

#include <inttypes.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common/app_log.h"

#define HEALTH_SAMPLE_PERIOD_MS 1000
#define HEALTH_STACK_WARN_WORDS 256

static const char *TAG = "app_health";
static app_health_task_refs_t s_refs;
static temperature_sensor_handle_t s_temperature_sensor;

static void report_task(const char *name, TaskHandle_t task)
{
    if (task == NULL) {
        APP_LOGW(TAG, "task=%s handle=null", name);
        return;
    }

    const UBaseType_t high_water = uxTaskGetStackHighWaterMark(task);
    if (high_water < HEALTH_STACK_WARN_WORDS) {
        APP_LOGW(TAG, "task=%s stack_low_words=%u", name, (unsigned)high_water);
    } else {
        APP_LOGI(TAG, "task=%s stack_free_words=%u", name, (unsigned)high_water);
    }
}

static void health_task(void *arg)
{
    (void)arg;
    for (;;) {
        float temperature = 0.0f;
        const esp_err_t temperature_err = s_temperature_sensor != NULL
                                               ? temperature_sensor_get_celsius(
                                                     s_temperature_sensor, &temperature)
                                               : ESP_ERR_INVALID_STATE;
        const esp_err_t wdt_status = esp_task_wdt_status(NULL);
        APP_LOGI(TAG,
                 "health heap_free=%" PRIu32 " heap_min=%" PRIu32
                 " temp_err=%s temp=%.1fC wdt=%s",
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size(),
                 esp_err_to_name(temperature_err),
                 temperature,
                 esp_err_to_name(wdt_status));

        APP_LOGI(TAG, "task=main_task_startup stack_free_words=%u",
                 (unsigned)s_refs.main_stack_free_words);
        report_task("display_task", s_refs.display_task);
        report_task("sntp_task", s_refs.sntp_task);
        report_task("location_task", s_refs.location_task);
        report_task("weather_task", s_refs.weather_task);
        report_task("radar_presence", s_refs.radar_task);
        vTaskDelay(pdMS_TO_TICKS(HEALTH_SAMPLE_PERIOD_MS));
    }
}

esp_err_t app_health_init(const app_health_task_refs_t *refs)
{
    if (refs == NULL) return ESP_ERR_INVALID_ARG;
    s_refs = *refs;

    const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&config, &s_temperature_sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_temperature_sensor);
    }
    if (err != ESP_OK) {
        APP_LOGW(TAG, "chip temperature sensor unavailable: %s", esp_err_to_name(err));
        s_temperature_sensor = NULL;
    }

    if (xTaskCreate(health_task, "app_health", 4096, NULL, 2, NULL) != pdPASS) {
        if (s_temperature_sensor != NULL) {
            temperature_sensor_disable(s_temperature_sensor);
            temperature_sensor_uninstall(s_temperature_sensor);
            s_temperature_sensor = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
