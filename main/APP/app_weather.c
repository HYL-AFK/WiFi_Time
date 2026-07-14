#include "app_weather.h"
#include <string.h>
#include <time.h>
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "common/weather_snapshot.h"

#define WEATHER_NET_EVENT BIT0
static EventGroupHandle_t s_events;
static char s_body[2048]; static size_t s_len;
/* HTTP 分块回调：天气 JSON 较小，复制到固定缓存避免动态内存碎片。 */
static esp_err_t http_event(esp_http_client_event_t *event) { if(event->event_id==HTTP_EVENT_ON_DATA && s_len+event->data_len<sizeof(s_body)){memcpy(s_body+s_len,event->data,event->data_len);s_len+=event->data_len;s_body[s_len]=0;} return ESP_OK; }
/* 请求天气并原子替换天气快照；请求失败时不覆盖上一次有效数据。 */
static void refresh_weather(void)
{
    s_len=0; s_body[0]=0;
    esp_http_client_config_t cfg={.url="https://api.open-meteo.com/v1/forecast?latitude=31.23&longitude=121.47&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto",.event_handler=http_event,.timeout_ms=10000};
    esp_http_client_handle_t client=esp_http_client_init(&cfg); if(!client)return;
    if(esp_http_client_perform(client)==ESP_OK && esp_http_client_get_status_code(client)==200){
        cJSON *root=cJSON_Parse(s_body), *cur=root?cJSON_GetObjectItem(root,"current"):NULL;
        if(cur){weather_snapshot_t snap={0}; cJSON *v;
            v=cJSON_GetObjectItem(cur,"temperature_2m"); if(v)snap.temperature_c=(float)v->valuedouble;
            v=cJSON_GetObjectItem(cur,"relative_humidity_2m"); if(v)snap.humidity=v->valueint;
            v=cJSON_GetObjectItem(cur,"weather_code"); if(v)snap.weather_code=v->valueint;
            v=cJSON_GetObjectItem(cur,"wind_speed_10m"); if(v)snap.wind_kmh=(float)v->valuedouble;
            snap.valid=true;snap.updated_at=time(NULL);weather_snapshot_set(&snap);
        } cJSON_Delete(root);
    } esp_http_client_cleanup(client);
}
/* 天气任务：联网立即刷新，之后每 10 分钟等待一次刷新事件。 */
static void weather_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, WEATHER_NET_EVENT, pdTRUE,
                                               pdFALSE, pdMS_TO_TICKS(600000));
        /* 联网事件立即刷新；超时则执行下一次 10 分钟周期刷新。 */
        if ((bits & WEATHER_NET_EVENT) != 0 || bits == 0) {
            refresh_weather();
        }
    }
}
/* 初始化天气快照、事件组和 FreeRTOS 天气任务。 */
esp_err_t app_weather_init(void){if(weather_snapshot_init()!=ESP_OK)return ESP_ERR_NO_MEM;s_events=xEventGroupCreate();if(!s_events)return ESP_ERR_NO_MEM;return xTaskCreate(weather_task,"weather_task",6144,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;}
/* 网络状态发生变化时唤醒天气任务。 */
void app_weather_notify_network(void){if(s_events)xEventGroupSetBits(s_events,WEATHER_NET_EVENT);}
