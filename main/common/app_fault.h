#pragma once

/*
 * 统一异常管理与自愈框架。
 *
 * 每条异常都有固定编码（P 开头）、名称、等级，并记录到持久化的"异常寄存器表"：
 * - 等级 1（P0xx）：严重异常，按影响程度分档自愈（画面卡死 10 分钟、断网 30 分钟、
 *   天气失败 1 小时、日志写失败 2 小时），到点仍未恢复即自动重启设备。
 * - 等级 2（P1xx）：可长期挂载的异常（如低电量、RTC 不可用），只记录不重启。
 *
 * 寄存器表写入 NVS（命名空间 app_fault / 键 table），重启后保留累计次数与
 * 首末次发生时间，便于现场追溯；重启时活跃状态清零，由各模块重新检测。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    /* 等级 1：持续 4 小时触发自动重启。 */
    APP_FAULT_P001_WEATHER = 1,       /* 天气接口持续不可用 */
    APP_FAULT_P002_WIFI = 2,          /* Wi-Fi 连续连接失败 */
    APP_FAULT_P003_DISPLAY = 3,       /* 显示画面停滞或刷新失败 */
    APP_FAULT_P004_STORAGE = 4,       /* NVS 事件日志写入失败 */

    /* 等级 2：长期挂载，仅记录与提示。 */
    APP_FAULT_P101_RTC = 101,         /* DS3231 不可用 */
    APP_FAULT_P102_RADAR = 102,       /* 雷达传感器无有效数据 */
    APP_FAULT_P103_LOCATION = 103,    /* 定位在境外，疑似代理网络出口 */
    APP_FAULT_P104_LOW_BATTERY = 104, /* 低电量（预留，当前无电池供电） */

    APP_FAULT_CODE_COUNT = 8,
} app_fault_code_t;

/* 异常寄存器表的一行：编码、等级、活跃状态、累计次数与首末次时间戳。 */
typedef struct {
    uint16_t code;
    uint8_t level;
    uint8_t active;
    uint16_t raise_count;
    uint32_t first_seen_epoch;
    uint32_t last_seen_epoch;
} app_fault_entry_t;

/* 初始化异常表（从 NVS 恢复历史统计，活跃状态清零）。 */
esp_err_t app_fault_init(void);

/* 标记异常发生（幂等；转入活跃时写入寄存器表并打印 P 编码）。 */
void app_fault_raise(app_fault_code_t code);

/* 清除异常（恢复时调用；仅活跃状态变化才写寄存器表）。 */
void app_fault_clear(app_fault_code_t code);

bool app_fault_is_active(app_fault_code_t code);

/* 异常编码名，例如 "P001-WEATHER"。 */
const char *app_fault_name(app_fault_code_t code);

/* 把待写入的寄存器表落盘；由健康任务周期调用（启动敏感期自动推迟）。 */
void app_fault_flush_pending(void);

/* 等级 1 异常达到各自的自愈时限后重启设备；由健康任务周期调用。 */
void app_fault_check_auto_reboot(void);

/* 打印完整异常寄存器表。 */
void app_fault_dump(void);
