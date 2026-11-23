#include "app_health.h"

#include <inttypes.h>
#include <stdbool.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common/app_fault.h"
#include "common/app_log.h"
#include "common/runtime_event_log.h"
#include "app_display.h"

#define HEALTH_SAMPLE_PERIOD_MS 1000
#define HEALTH_STACK_WARN_WORDS 256
#define DISPLAY_STALL_TIMEOUT_MS 2000 /* 2 seconds */
/* 正常时主画面每 250 ms 至少强制刷新一次；长期无刷新说明画面被静默丢弃或传输停滞。 */
#define DISPLAY_FRAME_STALL_TIMEOUT_MS 10000

static const char *TAG = "app_health";
static app_health_task_refs_t s_refs;
static temperature_sensor_handle_t s_temperature_sensor;
static uint32_t s_last_display_heartbeat;
static bool s_display_stall_reported;
static bool s_display_frame_stall_reported;

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
    /* 健康任务自身也受看门狗保护，避免诊断能力随任务卡死一起丢失。 */
    (void)esp_task_wdt_add(NULL);
    for (;;) {
        (void)esp_task_wdt_reset();
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
        report_task("wifi_event_work", s_refs.wifi_event_work_task);
        report_task("radar_presence", s_refs.radar_task);

        app_display_health_t display_health = {0};
        app_display_get_health(&display_health);
        const TickType_t now = xTaskGetTickCount();
        if (display_health.heartbeat != s_last_display_heartbeat) {
            s_last_display_heartbeat = display_health.heartbeat;
            s_display_stall_reported = false;
        } else if (!s_display_stall_reported &&
                   display_health.last_progress_tick != 0 &&
                   (TickType_t)(now - display_health.last_progress_tick) >
                       pdMS_TO_TICKS(DISPLAY_STALL_TIMEOUT_MS)) {
            s_display_stall_reported = true;
            app_fault_raise(APP_FAULT_P003_DISPLAY);
            runtime_event_log_append(RUNTIME_EVENT_DISPLAY_ERROR,
                                     (int32_t)display_health.animation_frame,
                                     (int32_t)display_health.last_refresh_ms,
                                     DISPLAY_HEALTH_STALLED |
                                         ((display_health.animation_id & 0xffU)
                                          << DISPLAY_DIAGNOSTIC_ANIMATION_ID_SHIFT));
            APP_LOGW(TAG,
                     "display heartbeat stalled for 2 seconds: animation=%" PRIu32
                     " frame=%u last_refresh_ms=%" PRIu32,
                     display_health.animation_id,
                     display_health.animation_frame,
                     display_health.last_refresh_ms);
            app_display_request_recovery();
        }

        /* 心跳正常但画面长期没有刷新（例如主帧被诊断保护静默丢弃）同样需要恢复。 */
        if (display_health.last_refresh_tick != 0 &&
            (TickType_t)(now - display_health.last_refresh_tick) >
                pdMS_TO_TICKS(DISPLAY_FRAME_STALL_TIMEOUT_MS)) {
            if (!s_display_frame_stall_reported) {
                s_display_frame_stall_reported = true;
                app_fault_raise(APP_FAULT_P003_DISPLAY);
                runtime_event_log_append(RUNTIME_EVENT_DISPLAY_ERROR,
                                         (int32_t)display_health.animation_frame,
                                         (int32_t)display_health.last_refresh_ms,
                                         DISPLAY_FRAME_STALLED |
                                             ((display_health.animation_id & 0xffU)
                                              << DISPLAY_DIAGNOSTIC_ANIMATION_ID_SHIFT));
                APP_LOGW(TAG,
                         "display frame stalled: animation=%" PRIu32
                         " frame=%u last_refresh_ms=%" PRIu32,
                         display_health.animation_id,
                         display_health.animation_frame,
                         display_health.last_refresh_ms);
                app_display_request_recovery();
            }
        } else {
            s_display_frame_stall_reported = false;
            app_fault_clear(APP_FAULT_P003_DISPLAY);
        }

        /* 等级 1 异常持续满自愈时限后重启；异常表落盘避开启动敏感期。 */
        app_fault_flush_pending();
        app_fault_check_auto_reboot();
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
