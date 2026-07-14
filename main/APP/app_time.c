#include "app_time.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "app/app_rtc.h"
#include "common/time_snapshot.h"

#define TIME_NET_EVENT BIT0
static EventGroupHandle_t s_events;
static char s_timezone[64];
static size_t s_timezone_len;

/* HTTP 回调只负责把时区接口返回的文本复制到临时缓冲区。 */
static esp_err_t timezone_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && s_timezone_len + event->data_len < sizeof(s_timezone)) {
        memcpy(s_timezone + s_timezone_len, event->data, event->data_len);
        s_timezone_len += event->data_len;
        s_timezone[s_timezone_len] = '\0';
    }
    return ESP_OK;
}

/* 自动识别公网时区；失败时保留调用方设置的默认 CST-8。 */
static void refresh_timezone(void)
{
    s_timezone_len = 0; s_timezone[0] = '\0';
    esp_http_client_config_t cfg = {.url = "https://ipapi.co/timezone/", .event_handler = timezone_event, .timeout_ms = 8000};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    if (esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200 && s_timezone_len > 0) {
        while (s_timezone_len && (s_timezone[s_timezone_len - 1] == '\r' || s_timezone[s_timezone_len - 1] == '\n')) s_timezone[--s_timezone_len] = '\0';
        setenv("TZ", s_timezone, 1); tzset();
    }
    esp_http_client_cleanup(client);
}

/* 时间任务：收到联网事件后启动 SNTP，成功后更新系统时间和 DS3231。 */
static void time_task(void *arg)
{
    (void)arg; bool sntp_started=false;
    for (;;) {
        /* 事件组让网络回调不阻塞在 HTTPS/SNTP 操作上。 */
        EventBits_t bits=xEventGroupWaitBits(s_events,TIME_NET_EVENT,pdTRUE,pdFALSE,pdMS_TO_TICKS(1000));
        if ((bits & TIME_NET_EVENT) && !sntp_started) {
            refresh_timezone();
            esp_sntp_config_t cfg=ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org","ntp.aliyun.com"));
            if (esp_netif_sntp_init(&cfg)==ESP_OK) { esp_netif_sntp_start(); sntp_started=true; }
        }
        if (sntp_started && esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100)) == ESP_OK) {
            time_t now=time(NULL); struct tm local; localtime_r(&now,&local);
            time_snapshot_t snap={.local_time=local,.unix_time=now,.from_rtc=false,.from_sntp=true}; time_snapshot_set(&snap);
            app_rtc_set_from_system();
            sntp_started=false;
        }
    }
}

/* 默认使用中国时区，联网后由 refresh_timezone() 覆盖。 */
esp_err_t app_time_init(void) { setenv("TZ", "CST-8", 1); tzset(); s_events=xEventGroupCreate(); if(!s_events)return ESP_ERR_NO_MEM; return xTaskCreate(time_task,"time_task",4096,NULL,4,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM; }
/* Wi-Fi 状态回调只投递事件，由时间任务异步处理。 */
void app_time_notify_network(void) { if(s_events)xEventGroupSetBits(s_events,TIME_NET_EVENT); }
