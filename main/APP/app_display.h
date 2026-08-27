#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* 初始化 WS2812 并创建独占显示任务。 */
esp_err_t app_display_init(bool network_expected);
/* 投递显示刷新事件。 */
void app_display_request_refresh(void);
void app_display_set_brightness(uint8_t brightness_percent);
TaskHandle_t app_display_get_task_handle(void);
typedef struct {
    uint32_t heartbeat;
    TickType_t last_progress_tick;
    TickType_t last_refresh_tick;
    uint32_t last_refresh_ms;
    uint32_t animation_id;
    uint8_t animation_frame;
    uint32_t refresh_errors;
    uint32_t recovery_count;
    bool animation_active;
} app_display_health_t;
void app_display_get_health(app_display_health_t *out);
void app_display_request_recovery(void);
/* 同步 STA 是否已经获得 IP，供启动动画周期判定联网状态。 */
void app_display_set_network_connected(bool connected);
void app_display_set_wifi_fault(bool fault);
void app_display_set_weather_fault(bool fault);
void app_display_set_bluetooth_connected(bool connected);
void app_display_show_bluetooth_connected(void);
void app_display_set_weather_fetching(bool fetching);
void app_display_show_network_success(void);
void app_display_show_network_failure(void);
void app_display_show_weather_refresh_success(void);
void app_display_show_weather_refresh_failure(void);
