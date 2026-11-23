#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* 在读取本地 RTC 前设置默认时区，不启动任何网络任务。 */
esp_err_t app_time_prepare_timezone(void);
/* 创建 SNTP/时区校时任务。 */
esp_err_t app_time_init(void);
/* 投递联网事件，由校时任务异步处理。 */
void app_time_notify_network(void);
/* 应用用户手动配置的固定坐标（BluFi 配网），并立即触发天气刷新。 */
esp_err_t app_time_apply_fixed_location(double latitude, double longitude);
TaskHandle_t app_time_get_sntp_task_handle(void);
TaskHandle_t app_time_get_location_task_handle(void);
