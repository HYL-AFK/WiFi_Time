#pragma once
#include "esp_err.h"
/* 初始化 WS2812 并创建独占显示任务。 */
esp_err_t app_display_init(void);
/* 投递显示刷新事件。 */
void app_display_request_refresh(void);
