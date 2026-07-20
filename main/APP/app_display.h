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
/* 同步 STA 是否已经获得 IP，供启动动画周期判定联网状态。 */
void app_display_set_network_connected(bool connected);
void app_display_show_network_success(void);
void app_display_show_network_failure(void);
