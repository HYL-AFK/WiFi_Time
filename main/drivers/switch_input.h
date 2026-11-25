#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 拨动开关：GPIO8，低电平有效。拨动开关没有真正的“单击”，
   这里把任意一次电平翻转当作一次“单击”。 */
#define SWITCH_INPUT_GPIO 8
#define SWITCH_INPUT_DEBOUNCE_MS 30

esp_err_t switch_input_init(void);

/* 取出并清零自上次调用以来的翻转次数（已消抖）。 */
uint32_t switch_input_take_toggles(void);
