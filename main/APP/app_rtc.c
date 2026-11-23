#include "app_rtc.h"

#include <inttypes.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common/app_fault.h"
#include "common/time_snapshot.h"
#include "common/weather_snapshot.h"
#include "drivers/ds3231.h"
#include "app_log.h"

#define RTC_FAILURE_LOG_INTERVAL 10U
/* 连续读失败达到该次数视为 RTC 不可用，挂载等级 2 异常 P101。 */
#define RTC_FAULT_THRESHOLD 10U

static const char *TAG = "app_rtc";

static bool s_rtc_temperature_loaded;
static bool s_rtc_time_loaded;
static bool s_ds3231_ready;
static bool s_rtc_time_logged;
static uint32_t s_rtc_time_failures;
static uint32_t s_rtc_temperature_failures;

static bool should_log_failure(uint32_t failure_count)
{
    return failure_count == 1U ||
           (failure_count % RTC_FAILURE_LOG_INTERVAL) == 0U;
}

/* 读取 RTC 并直接写入快照，不对 RTC 时间做有效性判断。 */
static void read_rtc_to_snapshot(void)
{
    ds3231_time_t value = {0};
    esp_err_t err = ds3231_read(&value);
    if (err != ESP_OK) {
        s_rtc_time_failures++;
        if (s_rtc_time_failures >= RTC_FAULT_THRESHOLD) {
            app_fault_raise(APP_FAULT_P101_RTC);
        }
        if (should_log_failure(s_rtc_time_failures)) {
            APP_LOG_ERR(TAG, err, "RTC time read failed, consecutive=%" PRIu32,
                        s_rtc_time_failures);
        }
        return;
    }

    const bool recovered = s_rtc_time_failures > 0U;
    if (recovered) {
        APP_LOGI(TAG, "RTC time read recovered after %" PRIu32 " failures",
                 s_rtc_time_failures);
        s_rtc_time_failures = 0U;
    }
    app_fault_clear(APP_FAULT_P101_RTC);

    if (!s_rtc_time_logged || recovered) {
        ds3231_register_cache_t cache = {0};
        ds3231_get_register_cache(&cache);
        APP_LOGI(TAG,
                 "RTC time read OK: %04d-%02d-%02d %02d:%02d:%02d status=0x%02x cache_rev=%" PRIu32,
                 value.time.tm_year + 1900,
                 value.time.tm_mon + 1,
                 value.time.tm_mday,
                 value.time.tm_hour,
                 value.time.tm_min,
                 value.time.tm_sec,
                 cache.status_reg,
                 cache.revision);
        s_rtc_time_logged = true;
    }

    value.time.tm_isdst = -1;
    time_t now = mktime(&value.time);
    if (now == (time_t)-1) {
        APP_LOGE(TAG, "RTC local time conversion failed");
        return;
    }
    struct timeval tv = {.tv_sec = now, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        APP_LOGE(TAG, "set system time from RTC failed");
        return;
    }

    time_snapshot_t snap = {
        .local_time = value.time,
        .unix_time = now,
        .from_rtc = true,
        .from_sntp = false,
    };
    time_snapshot_set(&snap);
    s_rtc_time_loaded = true;
}

static void read_rtc_temperature_to_snapshot(void)
{
    int16_t quarter_degrees = 0;
    esp_err_t err = ds3231_read_temperature_q4(&quarter_degrees);
    if (err != ESP_OK) {
        s_rtc_temperature_failures++;
        if (should_log_failure(s_rtc_temperature_failures)) {
            APP_LOG_ERR(TAG, err, "RTC temperature read failed, consecutive=%" PRIu32,
                        s_rtc_temperature_failures);
        }
        return;
    }

    if (s_rtc_temperature_failures > 0U) {
        APP_LOGI(TAG, "RTC temperature read recovered after %" PRIu32 " failures",
                 s_rtc_temperature_failures);
        s_rtc_temperature_failures = 0U;
    }

    int32_t magnitude_q4 = quarter_degrees;
    const char *sign = "";
    if (magnitude_q4 < 0) {
        sign = "-";
        magnitude_q4 = -magnitude_q4;
    }
    APP_LOGI(TAG, "RTC temperature read OK: %s%" PRId32 ".%02" PRId32 " C",
             sign,
             magnitude_q4 / 4,
             (magnitude_q4 % 4) * 25);

    weather_snapshot_set_rtc_temperature_q4(quarter_degrees);
    s_rtc_temperature_loaded = true;
}

/* SNTP 校时前周期读取 RTC，保证断网时秒数继续变化。 */
static void rtc_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_ds3231_ready) {
            esp_err_t err = ds3231_init();
            if (err == ESP_OK) {
                s_ds3231_ready = true;
                APP_LOGI(TAG, "DS3231 initialization recovered");
            } else {
                APP_LOG_ERR(TAG, err, "DS3231 initialization retry failed");
            }
        }
        if (s_ds3231_ready && !s_rtc_time_loaded) {
            read_rtc_to_snapshot();
        }
        if (s_ds3231_ready && !s_rtc_temperature_loaded) {
            read_rtc_temperature_to_snapshot();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 初始化 DS3231，先完成一次同步读取，再创建 RTC 任务。 */
esp_err_t app_rtc_init(void)
{
    esp_err_t err = ds3231_init();
    if (err == ESP_OK) {
        s_ds3231_ready = true;
    } else {
        APP_LOG_ERR(TAG, err, "DS3231 init failed");
    }

    err = time_snapshot_init();
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "time snapshot init failed");
        return err;
    }

    if (s_ds3231_ready) {
        read_rtc_to_snapshot();
        read_rtc_temperature_to_snapshot();
    }
    if (xTaskCreate(rtc_task, "rtc_task", 3072, NULL, 5, NULL) != pdPASS) {
        APP_LOGE(TAG, "create rtc_task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_rtc_set_from_system(void)
{
    if (!s_ds3231_ready) {
        esp_err_t init_err = ds3231_init();
        if (init_err != ESP_OK) return init_err;
        s_ds3231_ready = true;
    }
    /* SNTP 成功后把本地时间写回 DS3231，供下一次冷启动使用。 */
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    esp_err_t err = ds3231_write(&local);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err,
                    "RTC writeback failed: %04d-%02d-%02d %02d:%02d:%02d",
                    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                    local.tm_hour, local.tm_min, local.tm_sec);
        return err;
    }

    APP_LOGI(TAG, "RTC writeback OK: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    return ESP_OK;
}
