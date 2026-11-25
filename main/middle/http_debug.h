#pragma once

#include "esp_err.h"

/*
 * 局域网调试用 HTTP 接口（仅调试阶段开启，量产前应关闭）。
 *
 *   GET /                 手机浏览器直接打开，页面上有按钮
 *   GET /page?n=0|1       0 = 主页面，1 = 闹钟页
 *   GET /alarm?on=0|1     闹钟开关（接闹钟服务前用于验收 UI）
 *
 * 依赖 Wi-Fi 已初始化（esp_netif 存在），应在 wifi_manager_init 之后调用。
 */
esp_err_t http_debug_start(void);

/* 关闭调试接口。 */
void http_debug_stop(void);
