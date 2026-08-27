#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_log.h"
#include "app_config.h"
#include "common/runtime_event_log.h"
#include "common/time_snapshot.h"
#include "common/weather_snapshot.h"
#include "app/app_rtc.h"
#include "app/app_time.h"
#include "app/app_weather.h"
#include "app/app_display.h"
#include "app/app_health.h"
#include "radar_presence.h"
#include "wifi_manager.h"
#include "ble_provision.h"

static const char *TAG = "witime";
static bool s_last_sta_connected;
static uint32_t s_last_sta_failure_count;
#define WITIME_RADAR_ENABLED 1
#define WITIME_RADAR_STANDALONE_TEST 0

#if WITIME_RADAR_ENABLED
static void radar_changed(bool detected, uint32_t distance_mm)
{
#if !WITIME_RADAR_STANDALONE_TEST
    app_display_set_brightness(detected ? 60U : 20U);
    APP_LOGI(TAG,
             "radar brightness=%u%% distance=%" PRIu32 "mm",
             detected ? 60U : 20U,
             distance_mm);
#else
    (void)detected;
    (void)distance_mm;
#endif
}
#endif

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
    }
}

static void log_startup_diagnostics(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    APP_LOGI(TAG,
             "startup reset_reason=%d(%s) heap_free=%u heap_min=%u",
             (int)reason,
             reset_reason_name(reason),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void wifi_changed(void)
{
    const bool connected = wifi_manager_is_sta_connected();
    const uint32_t failure_count = wifi_manager_get_sta_failure_count();

    app_display_set_network_connected(connected);
    if (connected && !s_last_sta_connected) {
        app_display_set_wifi_fault(false);
        app_display_show_network_success();
        app_time_notify_network();
    } else if (!connected &&
               failure_count == 3U &&
               s_last_sta_failure_count != failure_count) {
        app_display_set_wifi_fault(true);
        app_display_show_network_failure();
    }

    s_last_sta_connected = connected;
    s_last_sta_failure_count = failure_count;
    app_display_request_refresh();
}

static void apply_wifi_cfg(const app_config_t *cfg)
{
    wifi_manager_set_ap_name(cfg->ap_name);
    wifi_manager_set_ap_password(cfg->ap_pass);
    if (cfg->has_wifi && cfg->wifi_count > 0) {
        wifi_manager_connect_sta(cfg);
    }
}

void app_main(void)
{
#if WITIME_RADAR_STANDALONE_TEST
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("radar_presence", ESP_LOG_INFO);
#endif
    log_startup_diagnostics();
#if WITIME_RADAR_STANDALONE_TEST
    APP_LOGI(TAG, "standalone radar test mode: RTC/display/network disabled");
    esp_err_t radar_err = radar_presence_init();
    if (radar_err != ESP_OK) {
        APP_LOG_ERR(TAG, radar_err, "standalone radar init failed");
    } else {
        radar_presence_set_callback(radar_changed);
        APP_LOGI(TAG, "standalone radar test running; threshold=500mm");
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = runtime_event_log_init();
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "runtime event log init failed; continuing without event storage");
    } else {
        runtime_event_log_append(RUNTIME_EVENT_BOOT,
                                 (int32_t)esp_reset_reason(),
                                 (int32_t)esp_get_free_heap_size(),
                                 0);
    }

    app_config_t cfg;
    app_config_load(&cfg);

    ESP_ERROR_CHECK(time_snapshot_init());
    ESP_ERROR_CHECK(weather_snapshot_init());
    ESP_ERROR_CHECK(app_time_prepare_timezone());
    ESP_ERROR_CHECK(app_rtc_init());
    ESP_ERROR_CHECK(app_time_init());
    ESP_ERROR_CHECK(app_display_init(cfg.has_wifi && cfg.wifi_count > 0));
    ESP_ERROR_CHECK(app_weather_init());

 #if WITIME_RADAR_ENABLED
    app_display_set_brightness(20U);
    err = radar_presence_init();
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "radar presence init failed; continuing without sensor");
    }
    radar_presence_set_callback(radar_changed);
 #else
    app_display_set_brightness(60U);
    APP_LOGI(TAG, "radar disabled by WITIME_RADAR_ENABLED=0");
 #endif

    wifi_manager_set_status_cb(wifi_changed);
    err = wifi_manager_init(&cfg);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "Wi-Fi manager init failed");
        return;
    }
    ble_provision_init(&cfg, apply_wifi_cfg);

    if (cfg.has_wifi && cfg.wifi_count > 0) {
        wifi_manager_connect_sta(&cfg);
    } else {
        APP_LOGI(TAG, "no saved Wi-Fi profile; APSTA/BluFi remain available");
    }

    const app_health_task_refs_t health_refs = {
        .main_stack_free_words = uxTaskGetStackHighWaterMark(NULL),
        .display_task = app_display_get_task_handle(),
        .sntp_task = app_time_get_sntp_task_handle(),
        .location_task = app_time_get_location_task_handle(),
        .weather_task = app_weather_get_task_handle(),
        .wifi_event_work_task = wifi_manager_get_event_work_task_handle(),
        .radar_task =
 #if WITIME_RADAR_ENABLED
            radar_presence_get_task_handle(),
 #else
            NULL,
 #endif
    };
    err = app_health_init(&health_refs);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "health monitor init failed; continuing without diagnostics");
    }

    APP_LOGI(TAG, "main task stack high-water mark: %u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#endif
}
