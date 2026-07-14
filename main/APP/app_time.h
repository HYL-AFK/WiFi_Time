#pragma once
#include "esp_err.h"
/* 创建 SNTP/时区校时任务。 */
esp_err_t app_time_init(void);
/* 投递联网事件，由校时任务异步处理。 */
void app_time_notify_network(void);
