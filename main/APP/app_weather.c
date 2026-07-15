#include "app_weather.h"

#include <stdbool.h>
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
#include "wifi_manager.h"

#define WEATHER_NET_EVENT BIT0

static EventGroupHandle_t s_events;
static char s_body[2048];
static size_t s_len;
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
    }
    return ESP_OK;
}

static void refresh_weather(void)
{
    s_len = 0;
    s_body[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = "https://api.open-meteo.com/v1/forecast?latitude=31.23&longitude=121.47&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto",
        .event_handler = http_event,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return;

    if (esp_http_client_perform(client) == ESP_OK &&
        esp_http_client_get_status_code(client) == 200) {
        cJSON *root = cJSON_Parse(s_body);
        cJSON *current = root != NULL ? cJSON_GetObjectItem(root, "current") : NULL;

        if (current != NULL) {
            weather_snapshot_t snapshot = {0};
            cJSON *value;
            bool has_temperature = false;
            bool has_humidity = false;
            bool has_weather_code = false;

            value = cJSON_GetObjectItem(current, "temperature_2m");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.temperature_c = (float)value->valuedouble;
                has_temperature = true;
            }

            value = cJSON_GetObjectItem(current, "relative_humidity_2m");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.humidity = value->valueint;
                has_humidity = true;
            }

            value = cJSON_GetObjectItem(current, "weather_code");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.weather_code = value->valueint;
                has_weather_code = true;
            }

            value = cJSON_GetObjectItem(current, "wind_speed_10m");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.wind_kmh = (float)value->valuedouble;
            }

            /* Only a complete weather response may replace the RTC fallback. */
            if (has_temperature && has_humidity && has_weather_code) {
                snapshot.valid = true;
                snapshot.temperature_valid = true;
                snapshot.temperature_source = WEATHER_TEMP_SOURCE_NETWORK;
                snapshot.updated_at = time(NULL);
                weather_snapshot_set(&snapshot);
                APP_LOGI(TAG,
                         "weather code=%d condition=%s temperature_x10=%d humidity=%d",
                         snapshot.weather_code,
                         weather_condition_name(snapshot.weather_code),
                         (int)(snapshot.temperature_c * 10.0f),
                         snapshot.humidity);
            }
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
    return xTaskCreate(weather_task, "weather_task", 6144, NULL, 3, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void app_weather_notify_network(void)
{
    if (s_events != NULL) xEventGroupSetBits(s_events, WEATHER_NET_EVENT);
}
