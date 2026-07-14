#pragma once
#include "esp_err.h"
/* 初始化 RTC 任务，并优先从 DS3231 恢复启动时间。 */
esp_err_t app_rtc_init(void);
/* SNTP 校时后，将当前系统时间写回 DS3231。 */
esp_err_t app_rtc_set_from_system(void);
