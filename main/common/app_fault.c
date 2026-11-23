#include "app_fault.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs.h"

#include "app_log.h"

#define APP_FAULT_NVS_NS "app_fault"
#define APP_FAULT_NVS_KEY "table"
#define APP_FAULT_MINUTE_MS (60 * 1000U)
/* 异常表持久化延后到启动 30 秒后：flash 擦写会阻塞 cache/中断数十毫秒，
   启动动画期间的 WS2812 刷新不能被它拖慢或打断。 */
#define APP_FAULT_PERSIST_DELAY_MS (30 * 1000U)
#define APP_FAULT_TIME_VALID_EPOCH 1577836800LL

static const char *TAG = "app_fault";

static const uint16_t s_codes[APP_FAULT_CODE_COUNT] = {
    APP_FAULT_P001_WEATHER,
    APP_FAULT_P002_WIFI,
    APP_FAULT_P003_DISPLAY,
    APP_FAULT_P004_STORAGE,
    APP_FAULT_P101_RTC,
    APP_FAULT_P102_RADAR,
    APP_FAULT_P103_LOCATION,
    APP_FAULT_P104_LOW_BATTERY,
};

static const char *const s_names[APP_FAULT_CODE_COUNT] = {
    "P001-WEATHER",
    "P002-WIFI",
    "P003-DISPLAY",
    "P004-STORAGE",
    "P101-RTC",
    "P102-RADAR",
    "P103-LOCATION",
    "P104-LOW_BATTERY",
};

/* 各编码的自愈时限（分钟）：按影响程度分档，到点仍未恢复即重启自愈。
   0 表示长期挂载（等级 2），只记录不重启。 */
static const uint32_t s_self_heal_min[APP_FAULT_CODE_COUNT] = {
    60U,  /* P001 WEATHER：天气接口持续失败，1 小时 */
    30U,  /* P002 WIFI：连续连不上，30 分钟 */
    10U,  /* P003 DISPLAY：画面卡死，10 分钟（最影响使用，最快自愈） */
    120U, /* P004 STORAGE：事件日志写失败，2 小时 */
    0U,   /* P101 RTC：长期挂载 */
    0U,   /* P102 RADAR：长期挂载 */
    0U,   /* P103 LOCATION：长期挂载 */
    0U,   /* P104 LOW_BATTERY：长期挂载 */
};

static app_fault_entry_t s_table[APP_FAULT_CODE_COUNT];
/* 等级 1 异常的本次运行起始 tick，用于判定是否已经持续满自愈时限。 */
static TickType_t s_active_since[APP_FAULT_CODE_COUNT];
/* 异常表有变更待写入 NVS（延后写，避开显示刷新敏感期）。 */
static bool s_persist_dirty;
static bool s_initialized;

static int fault_index(uint16_t code)
{
    for (int i = 0; i < APP_FAULT_CODE_COUNT; ++i) {
        if (s_codes[i] == code) return i;
    }
    return -1;
}

static esp_err_t app_fault_persist(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_FAULT_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, APP_FAULT_NVS_KEY, s_table, sizeof(s_table));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_fault_init(void)
{
    if (s_initialized) return ESP_OK;

    memset(s_table, 0, sizeof(s_table));
    memset(s_active_since, 0, sizeof(s_active_since));
    for (int i = 0; i < APP_FAULT_CODE_COUNT; ++i) {
        s_table[i].code = s_codes[i];
        s_table[i].level = (s_codes[i] < 100U) ? 1U : 2U;
    }

    nvs_handle_t handle;
    if (nvs_open(APP_FAULT_NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
        app_fault_entry_t loaded[APP_FAULT_CODE_COUNT];
        size_t len = sizeof(loaded);
        if (nvs_get_blob(handle, APP_FAULT_NVS_KEY, loaded, &len) == ESP_OK &&
            len == sizeof(loaded)) {
            /* 恢复历史统计；活跃状态清零，重启后由各模块重新检测。 */
            for (int i = 0; i < APP_FAULT_CODE_COUNT; ++i) {
                if (loaded[i].code != s_codes[i]) continue;
                s_table[i].raise_count = loaded[i].raise_count;
                s_table[i].first_seen_epoch = loaded[i].first_seen_epoch;
                s_table[i].last_seen_epoch = loaded[i].last_seen_epoch;
            }
        }
        nvs_close(handle);
    }

    s_initialized = true;
    APP_LOGI(TAG, "fault register ready: %d codes, self-heal 10m/30m/1h/2h",
             (int)APP_FAULT_CODE_COUNT);
    app_fault_dump();
    return ESP_OK;
}

void app_fault_raise(app_fault_code_t code)
{
    if (!s_initialized) return;
    const int idx = fault_index((uint16_t)code);
    if (idx < 0) return;

    app_fault_entry_t *entry = &s_table[idx];
    const time_t now = time(NULL);
    const bool time_valid = now >= (time_t)APP_FAULT_TIME_VALID_EPOCH;

    if (!entry->active) {
        entry->active = 1;
        s_active_since[idx] = xTaskGetTickCount();
        entry->raise_count++;
        if (entry->raise_count == 0) entry->raise_count = 1;
        if (time_valid) entry->first_seen_epoch = (uint32_t)now;
        if (time_valid) entry->last_seen_epoch = (uint32_t)now;
        APP_LOGW(TAG, "fault raised: %s level=%u count=%u self_heal=%umin",
                 s_names[idx], (unsigned)entry->level,
                 (unsigned)entry->raise_count,
                 (unsigned)s_self_heal_min[idx]);
        /* 标记待写，由健康任务延后落盘，避免 flash 擦写打断显示刷新。 */
        s_persist_dirty = true;
        return;
    }

    if (time_valid) entry->last_seen_epoch = (uint32_t)now;
}

void app_fault_clear(app_fault_code_t code)
{
    if (!s_initialized) return;
    const int idx = fault_index((uint16_t)code);
    if (idx < 0 || !s_table[idx].active) return;

    s_table[idx].active = 0;
    APP_LOGI(TAG, "fault cleared: %s", s_names[idx]);
    s_persist_dirty = true;
}

bool app_fault_is_active(app_fault_code_t code)
{
    const int idx = fault_index((uint16_t)code);
    return idx >= 0 && s_table[idx].active != 0;
}

const char *app_fault_name(app_fault_code_t code)
{
    const int idx = fault_index((uint16_t)code);
    return idx >= 0 ? s_names[idx] : "P???";
}

void app_fault_flush_pending(void)
{
    if (!s_initialized || !s_persist_dirty) return;
    /* 启动后的显示敏感期（启动动画/首次主帧）内不落盘。 */
    if (xTaskGetTickCount() < pdMS_TO_TICKS(APP_FAULT_PERSIST_DELAY_MS)) return;
    s_persist_dirty = false;
    (void)app_fault_persist();
}

void app_fault_check_auto_reboot(void)
{
    if (!s_initialized) return;

    const TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < APP_FAULT_CODE_COUNT; ++i) {
        const uint32_t limit_min = s_self_heal_min[i];
        if (limit_min == 0U || !s_table[i].active) continue;
        if ((TickType_t)(now - s_active_since[i]) <
            pdMS_TO_TICKS(limit_min * APP_FAULT_MINUTE_MS)) {
            continue;
        }

        APP_LOGE(TAG, "fault %s active for %u min; rebooting for self-healing",
                 s_names[i], (unsigned)limit_min);
        (void)app_fault_persist();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

void app_fault_dump(void)
{
    for (int i = 0; i < APP_FAULT_CODE_COUNT; ++i) {
        if (s_self_heal_min[i] == 0U) {
            APP_LOGI(TAG,
                     "fault[%d] %s level=%u active=%u count=%u first=%u last=%u self_heal=permanent",
                     i, s_names[i], (unsigned)s_table[i].level,
                     (unsigned)s_table[i].active,
                     (unsigned)s_table[i].raise_count,
                     (unsigned)s_table[i].first_seen_epoch,
                     (unsigned)s_table[i].last_seen_epoch);
        } else {
            APP_LOGI(TAG,
                     "fault[%d] %s level=%u active=%u count=%u first=%u last=%u self_heal=%umin",
                     i, s_names[i], (unsigned)s_table[i].level,
                     (unsigned)s_table[i].active,
                     (unsigned)s_table[i].raise_count,
                     (unsigned)s_table[i].first_seen_epoch,
                     (unsigned)s_table[i].last_seen_epoch,
                     (unsigned)s_self_heal_min[i]);
        }
    }
}
