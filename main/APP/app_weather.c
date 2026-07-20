#include "app_weather.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "app_log.h"
#include "common/weather_snapshot.h"
#include "common/location_snapshot.h"
#include "common/runtime_event_log.h"
#include "wifi_manager.h"

#define WEATHER_NET_EVENT BIT0

static EventGroupHandle_t s_events;
static char s_body[2048];
static size_t s_len;
static bool s_body_overflow;
static TaskHandle_t s_weather_task_handle;
static const char *TAG = "app_weather";

static const char *weather_condition_name(int weather_code)
{
    if (weather_code == 0) return "clear";
    if ((weather_code >= 1 && weather_code <= 3) ||
        weather_code == 45 || weather_code == 48) return "cloudy";
    if ((weather_code >= 51 && weather_code <= 67) ||
        (weather_code >= 80 && weather_code <= 82)) return "rain";
    if ((weather_code >= 71 && weather_code <= 77) ||
        (weather_code >= 85 && weather_code <= 86)) return "snow";
    if (weather_code >= 95 && weather_code <= 99) return "thunder";
    return "unknown";
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL &&
        s_len + event->data_len < sizeof(s_body)) {
        memcpy(s_body + s_len, event->data, event->data_len);
        s_len += event->data_len;
        s_body[s_len] = '\0';
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL) {
        s_body_overflow = true;
    }
    return ESP_OK;
}

static void refresh_weather(void)
{
    s_len = 0;
    s_body[0] = '\0';
    s_body_overflow = false;

    location_snapshot_t location = {0};
    /* 定位失败时不使用伪造坐标，保留上一次有效天气或 RTC 温度。 */
    if (!location_snapshot_get(&location)) {
        APP_LOGW(TAG, "skip weather request: no valid location");
        return;
    }

    char url[512];
    int url_len = snprintf(
        url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,weather_code&timezone=auto",
        location.latitude, location.longitude);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        APP_LOGE(TAG, "weather URL buffer is too small");
        return;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return;

    esp_err_t request_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (request_err != ESP_OK) {
        APP_LOG_ERR(TAG, request_err, "weather request failed, status=%d", status);
        runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, request_err, 0);
    } else if (s_body_overflow) {
        APP_LOGE(TAG, "weather response overflow, status=%d", status);
        runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 1, 0);
    } else if (status != 200) {
        APP_LOGW(TAG, "weather request returned HTTP %d", status);
        runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 2, 0);
    } else {
        cJSON *root = cJSON_Parse(s_body);
        cJSON *current = root != NULL ? cJSON_GetObjectItem(root, "current") : NULL;

        if (root == NULL) {
            APP_LOGW(TAG, "weather response JSON parse failed");
        } else if (current != NULL) {
            weather_snapshot_t snapshot = {0};
            cJSON *value;
            bool has_temperature = false;
            bool has_weather_code = false;

            value = cJSON_GetObjectItem(current, "temperature_2m");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.temperature_c = (float)value->valuedouble;
                has_temperature = true;
            }

            value = cJSON_GetObjectItem(current, "weather_code");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.weather_code = value->valueint;
                has_weather_code = true;
            }

            /* 只有完整响应才能覆盖 RTC 温度回退和上一份网络天气。 */
            if (has_temperature && has_weather_code) {
                snapshot.valid = true;
                snapshot.temperature_valid = true;
                snapshot.temperature_source = WEATHER_TEMP_SOURCE_NETWORK;
                snapshot.updated_at = time(NULL);
                weather_snapshot_set(&snapshot);
                APP_LOGI(TAG,
                         "weather code=%d condition=%s temperature_x10=%d",
                         snapshot.weather_code,
                         weather_condition_name(snapshot.weather_code),
                         (int)(snapshot.temperature_c * 10.0f));
                runtime_event_log_append(RUNTIME_EVENT_WEATHER_SUCCESS,
                                         snapshot.weather_code,
                                         (int32_t)(snapshot.temperature_c * 10.0f),
                                         0);
            } else {
                APP_LOGW(TAG, "weather JSON missing required current fields");
                runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 3, 0);
            }
        } else {
            APP_LOGW(TAG, "weather JSON missing current object");
            runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 4, 0);
        }
        cJSON_Delete(root);
    }

    esp_http_client_cleanup(client);
}

static void weather_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, WEATHER_NET_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(600000));
        if ((bits & WEATHER_NET_EVENT) != 0 || bits == 0) {
            if (!wifi_manager_is_sta_connected()) {
                continue;
            }
            refresh_weather();
        }
    }
}

esp_err_t app_weather_init(void)
{
    if (weather_snapshot_init() != ESP_OK) return ESP_ERR_NO_MEM;
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    return xTaskCreate(weather_task, "weather_task", 6144, NULL, 3,
                       &s_weather_task_handle) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

TaskHandle_t app_weather_get_task_handle(void)
{
    return s_weather_task_handle;
}

void app_weather_notify_network(void)
{
    if (s_events != NULL) xEventGroupSetBits(s_events, WEATHER_NET_EVENT);
}
