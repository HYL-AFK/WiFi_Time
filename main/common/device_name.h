#pragma once

/*
 * 设备统一名称：<主机名>_<WiFi STA MAC 后六位>。
 *
 * 蓝牙广播名与 WiFi 热点（SoftAP）名共用同一个字符串，例如 ESPARK_EA01D8。
 * 手机扫描蓝牙或 Wi-Fi 时看到的名字一致，多台设备可以直观区分。
 */

/* 返回静态缓冲区中的设备名，首次调用时生成。 */
const char *device_name_get(void);
