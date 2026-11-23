#include "app_weather.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "app_display.h"
#include "app_log.h"
#include "common/app_fault.h"
#include "common/weather_snapshot.h"
#include "common/location_snapshot.h"
#include "common/runtime_event_log.h"
#include "wifi_manager.h"

#define WEATHER_NET_EVENT BIT0
#define WEATHER_FIRST_WINDOW_START_MS (20 * 60 * 1000)
#define WEATHER_FETCH_WINDOW_MS (10 * 60 * 1000)
#define WEATHER_REFRESH_PERIOD_MS (30 * 60 * 1000)
/* 窗口内失败后每隔一分钟重试一次，避免一次瞬时网络抖动导致整个窗口判定失败。 */
#define WEATHER_WINDOW_RETRY_MS (60 * 1000)
/* 开机首次请求（定位未就绪/网络未连）失败多次后仍未成功，才点亮天气故障灯。 */
#define WEATHER_INITIAL_FAULT_THRESHOLD 5U

static EventGroupHandle_t s_events;
static char s_body[2048];
static size_t s_len;
static bool s_body_overflow;
static TaskHandle_t s_weather_task_handle;
static TickType_t s_weather_task_started;
static bool s_initial_request_done;
static uint8_t s_initial_fail_count;
static bool s_window_active;
static bool s_window_success;
static uint32_t s_window_index;
static TickType_t s_window_end_tick;
static TickType_t s_window_last_attempt_tick;
static bool s_publish_failed;
static weather_snapshot_t s_pending_snapshot;
static bool s_pending_snapshot_valid;
static bool try_weather_request(void);
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

static bool refresh_weather(weather_snapshot_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
    app_display_set_weather_fetching(true);
    s_len = 0;
    s_body[0] = '\0';
    s_body_overflow = false;

    location_snapshot_t location = {0};
    /* 定位失败时不使用伪造坐标，保留上一次有效天气或 RTC 温度。 */
    if (!location_snapshot_get(&location)) {
        APP_LOGW(TAG, "skip weather request: no valid location");
        app_display_set_weather_fetching(false);
        return false;
    }

    char url[512];
    int url_len = snprintf(
        url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,weather_code&timezone=auto",
        location.latitude, location.longitude);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        APP_LOGE(TAG, "weather URL buffer is too small");
        app_display_set_weather_fetching(false);
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        app_display_set_weather_fetching(false);
        return false;
    }

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
                if (out != NULL) *out = snapshot;
                APP_LOGI(TAG,
                         "weather code=%d condition=%s temperature_x10=%d",
                         snapshot.weather_code,
                         weather_condition_name(snapshot.weather_code),
                         (int)(snapshot.temperature_c * 10.0f));
                runtime_event_log_append(RUNTIME_EVENT_WEATHER_SUCCESS,
                                         snapshot.weather_code,
                                         (int32_t)(snapshot.temperature_c * 10.0f),
                                         0);
                esp_http_client_cleanup(client);
                app_display_set_weather_fetching(false);
                return true;
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
    app_display_set_weather_fetching(false);
    return false;
}

static void publish_pending_weather(void)
{
    if (s_pending_snapshot_valid) {
        weather_snapshot_set(&s_pending_snapshot);
        s_pending_snapshot_valid = false;
        s_publish_failed = false;
        app_fault_clear(APP_FAULT_P001_WEATHER);
        app_display_set_weather_fault(false);
        app_display_show_weather_refresh_success();
        return;
    }

    /* 失败不覆盖快照：温度和天气图标沿用上一次成功数据，只点亮故障灯提示。 */
    s_publish_failed = true;
    app_fault_raise(APP_FAULT_P001_WEATHER);
    app_display_set_weather_fault(true);
    app_display_show_weather_refresh_failure();
}

/* 开机首次请求：成功后立即发布并结束首次流程；失败则交给每分钟重试。 */
static void publish_initial_weather(void)
{
    if (try_weather_request() && s_pending_snapshot_valid) {
        weather_snapshot_t initial = s_pending_snapshot;
        s_pending_snapshot_valid = false;
        weather_snapshot_set(&initial);
        app_fault_clear(APP_FAULT_P001_WEATHER);
        app_display_set_weather_fault(false);
        s_initial_fail_count = 0;
        s_initial_request_done = true;
        return;
    }

    /* 开机阶段定位/网络尚未就绪时请求必然失败，累计多次后再点亮故障灯。 */
    if (s_initial_fail_count < UINT8_MAX) s_initial_fail_count++;
    if (s_initial_fail_count >= WEATHER_INITIAL_FAULT_THRESHOLD) {
        s_publish_failed = true;
        app_fault_raise(APP_FAULT_P001_WEATHER);
        app_display_set_weather_fault(true);
    }
}

static bool try_weather_request(void)
{
    if (!wifi_manager_is_sta_connected()) return false;

    weather_snapshot_t snapshot = {0};
    if (!refresh_weather(&snapshot)) return false;

    s_pending_snapshot = snapshot;
    s_pending_snapshot_valid = true;
    s_window_success = true;
    return true;
}

static void start_weather_window(uint32_t index, TickType_t start_tick)
{
    s_window_index = index;
    s_window_active = true;
    s_window_success = false;
    s_pending_snapshot_valid = false;
    s_window_end_tick = start_tick + pdMS_TO_TICKS(WEATHER_FETCH_WINDOW_MS);
    s_window_last_attempt_tick = start_tick;
    (void)try_weather_request();
}

static void weather_task(void *arg)
{
    (void)arg;
    s_weather_task_started = xTaskGetTickCount();
    s_window_index = UINT32_MAX;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, WEATHER_NET_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        const TickType_t now = xTaskGetTickCount();
        const TickType_t elapsed = now - s_weather_task_started;

        if ((bits & WEATHER_NET_EVENT) != 0) {
            if (!s_initial_request_done) {
                publish_initial_weather();
            } else if (s_window_active && !s_window_success) {
                s_window_last_attempt_tick = now;
                (void)try_weather_request();
            }
        }

        /* 首次请求未完成、窗口内未成功、或窗口外上次发布失败时，每分钟重试一次：
           网络或定位恢复后最多一分钟内补齐数据并清除故障灯。 */
        const bool initial_retry_needed = !s_initial_request_done;
        const bool window_retry_needed = s_window_active && !s_window_success;
        const bool recovery_retry_needed = !s_window_active && s_publish_failed;
        if ((initial_retry_needed || window_retry_needed || recovery_retry_needed) &&
            (int32_t)(now - s_window_last_attempt_tick) >=
                (int32_t)pdMS_TO_TICKS(WEATHER_WINDOW_RETRY_MS)) {
            s_window_last_attempt_tick = now;
            if (initial_retry_needed) {
                publish_initial_weather();
            } else if (try_weather_request() && recovery_retry_needed) {
                /* 窗口外的恢复重试成功后立即发布，清除红叉与故障灯。 */
                publish_pending_weather();
            }
        }

        if (!s_window_active &&
            elapsed >= pdMS_TO_TICKS(WEATHER_FIRST_WINDOW_START_MS)) {
            const uint32_t index = (uint32_t)((elapsed -
                                               pdMS_TO_TICKS(WEATHER_FIRST_WINDOW_START_MS)) /
                                              pdMS_TO_TICKS(WEATHER_REFRESH_PERIOD_MS));
            if (s_window_index == UINT32_MAX || index > s_window_index) {
                const TickType_t start_tick = s_weather_task_started +
                                              pdMS_TO_TICKS(WEATHER_FIRST_WINDOW_START_MS) +
                                              index * pdMS_TO_TICKS(WEATHER_REFRESH_PERIOD_MS);
                start_weather_window(index, start_tick);
            }
        }

        if (s_window_active && (int32_t)(now - s_window_end_tick) >= 0) {
            publish_pending_weather();
            s_window_active = false;
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
