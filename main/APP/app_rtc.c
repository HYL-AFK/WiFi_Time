#include "app_rtc.h"

#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common/time_snapshot.h"
#include "drivers/ds3231.h"

/* 读取 RTC 并直接写入快照，不对 RTC 时间做有效性判断。 */
static void read_rtc_to_snapshot(void)
{
    ds3231_time_t value = {0};
    if (ds3231_read(&value) != ESP_OK) {
        return;
    }

    time_t now = mktime(&value.time);
    struct timeval tv = {.tv_sec = now, .tv_usec = 0};
    settimeofday(&tv, NULL);

    time_snapshot_t snap = {
        .local_time = value.time,
        .unix_time = now,
        .from_rtc = true,
        .from_sntp = false,
    };
    time_snapshot_set(&snap);
}

/* SNTP 校时前周期读取 RTC，保证断网时秒数继续变化。 */
static void rtc_task(void *arg)
{
    (void)arg;
    for (;;) {
        time_snapshot_t snap = {0};
        time_snapshot_get(&snap);
        if (!snap.from_sntp) {
            read_rtc_to_snapshot();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 初始化 DS3231，先完成一次同步读取，再创建 RTC 任务。 */
esp_err_t app_rtc_init(void)
{
    esp_err_t err = ds3231_init();
    if (err != ESP_OK) return err;

    err = time_snapshot_init();
    if (err != ESP_OK) return err;

    read_rtc_to_snapshot();
    return xTaskCreate(rtc_task, "rtc_task", 3072, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t app_rtc_set_from_system(void)
{
    /* SNTP 成功后把本地时间写回 DS3231，供下一次冷启动使用。 */
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return ds3231_write(&local);
}
