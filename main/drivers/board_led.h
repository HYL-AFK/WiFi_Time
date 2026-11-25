#pragma once

#include "esp_err.h"

/*
 * 板载 WS2812 状态灯（ESP32-S3-Zero 板载，GPIO21，单颗 RGB）。
 *
 * 灯语：
 * - 正常工作：绿灯 300ms 亮 / 700ms 灭（周期 1 秒）
 * - 异常：红灯常亮
 *
 * 判定依据：Wi-Fi 已连上 STA 且 DS3231 未报故障（P101）。
 * 走独立 RMT 通道，与 16×16 点阵的 SPI 刷新互不干扰。
 */
esp_err_t board_led_init(void);
