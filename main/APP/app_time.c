#include "app_time.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app/app_rtc.h"
#include "app/app_weather.h"
#include "common/app_log.h"
#include "common/location_snapshot.h"
#include "common/time_snapshot.h"
#include "common/runtime_event_log.h"

#define TIME_NET_EVENT BIT0
#define LOCATION_BODY_MAX 2048
#define LOCATION_CHANGE_THRESHOLD_DEGREES 0.05
#define SNTP_SERVER_TIMEOUT_MS 3000
#define TIME_RESYNC_PERIOD_MS (4 * 60 * 60 * 1000)
#define SNTP_SYNC_ATTEMPT_TIMEOUT_MS 10000

static EventGroupHandle_t s_sntp_events;
static EventGroupHandle_t s_location_events;
static char s_location_body[LOCATION_BODY_MAX];
static size_t s_location_len;
static bool s_location_body_overflow;
static bool s_sntp_initialized;
static bool s_sntp_sync_applied;
static bool s_sntp_fallback_logged;
static bool s_sntp_cycle_active;
static bool s_timezone_prepared;
static int64_t s_sntp_started_us;
static int64_t s_next_sntp_due_us;
static TaskHandle_t s_sntp_task_handle;
static TaskHandle_t s_location_task_handle;
static const char *TAG = "app_time";

esp_err_t app_time_prepare_timezone(void)
{
    if (s_timezone_prepared) return ESP_OK;
    if (setenv("TZ", "CST-8", 1) != 0) {
        APP_LOGE(TAG, "set default timezone failed");
        return ESP_FAIL;
    }
    tzset();
    s_timezone_prepared = true;
    return ESP_OK;
}

static esp_err_t location_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len == 0) {
        return ESP_OK;
    }

    if (s_location_len + event->data_len >= sizeof(s_location_body)) {
        s_location_body_overflow = true;
        return ESP_OK;
    }

    memcpy(s_location_body + s_location_len, event->data, event->data_len);
    s_location_len += event->data_len;
    s_location_body[s_location_len] = '\0';
    return ESP_OK;
}

static bool get_location_value(cJSON *root, const char *name, double *out)
{
    cJSON *value = cJSON_GetObjectItem(root, name);
    if (value == NULL || !cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
        return false;
    }
    *out = value->valuedouble;
    return true;
}

static const char *get_location_timezone(cJSON *root)
{
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    if (timezone != NULL && cJSON_IsString(timezone)) {
        return timezone->valuestring;
    }
    if (timezone != NULL && cJSON_IsObject(timezone)) {
        cJSON *id = cJSON_GetObjectItem(timezone, "id");
        if (id != NULL && cJSON_IsString(id)) return id->valuestring;
    }
    return NULL;
}

static bool parse_location_response(bool nested_timezone, bool *changed)
{
    cJSON *root = cJSON_Parse(s_location_body);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        APP_LOGW(TAG, "location JSON parse failed");
        return false;
    }

    cJSON *success = cJSON_GetObjectItem(root, "success");
    if (success != NULL && cJSON_IsFalse(success)) {
        cJSON_Delete(root);
        APP_LOGW(TAG, "location provider reported failure");
        return false;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    const char *timezone = get_location_timezone(root);
    bool valid = get_location_value(root, "latitude", &latitude) &&
                 get_location_value(root, "longitude", &longitude) &&
                 timezone != NULL && timezone[0] != '\0' &&
                 latitude >= -90.0 && latitude <= 90.0 &&
                 longitude >= -180.0 && longitude <= 180.0;
    (void)nested_timezone;

    if (!valid) {
        if (changed != NULL) *changed = false;
        APP_LOGW(TAG, "location JSON missing valid coordinates or timezone");
        cJSON_Delete(root);
        return false;
    }

    location_snapshot_t previous = {0};
    const bool had_previous = location_snapshot_get(&previous);
    const bool location_changed =
        !had_previous ||
        fabs(latitude - previous.latitude) >= LOCATION_CHANGE_THRESHOLD_DEGREES ||
        fabs(longitude - previous.longitude) >= LOCATION_CHANGE_THRESHOLD_DEGREES ||
        strcmp(previous.timezone, timezone) != 0;

    bool ready = location_snapshot_set(latitude, longitude, timezone) == ESP_OK;
    if (ready) {
        APP_LOGI(TAG, "location updated: latitude=%.6f longitude=%.6f timezone=%s",
                 latitude, longitude, timezone);
        if (strcmp(timezone, "Asia/Shanghai") == 0) {
            if (app_time_prepare_timezone() == ESP_OK) {
                APP_LOGI(TAG, "timezone applied: Asia/Shanghai -> CST-8");
            }
        } else {
            APP_LOGW(TAG, "unsupported IANA timezone '%s', keep CST-8", timezone);
        }
    } else {
        APP_LOGW(TAG, "location JSON missing valid coordinates or timezone");
    }

    if (changed != NULL) *changed = ready && location_changed;
    cJSON_Delete(root);
    return ready;
}

static bool request_location(const char *url, bool nested_timezone, bool *changed)
{
    s_location_len = 0;
    s_location_body[0] = '\0';
    s_location_body_overflow = false;

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = location_http_event,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        APP_LOGE(TAG, "create location HTTPS client failed, url=%s", url);
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "WiTime/1.0");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool ready = false;
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "location request failed, status=%d url=%s", status, url);
    } else if (status != 200) {
        APP_LOGW(TAG, "location request returned HTTP %d url=%s", status, url);
    } else if (s_location_body_overflow) {
        APP_LOGE(TAG, "location response overflow, status=%d url=%s", status, url);
    } else {
        ready = parse_location_response(nested_timezone, changed);
    }

    esp_http_client_cleanup(client);
    return ready;
}

/* 主接口拒绝访问时使用备用接口，避免天气功能依赖单一定位服务。 */
static bool refresh_location(bool *changed)
{
    /* 优先使用主定位服务，失败后切换备用服务；已有位置缓存继续保留。 */
    if (changed != NULL) *changed = false;
    if (request_location("https://ipwho.is/", true, changed) ||
        request_location("https://ipapi.co/json/", false, changed)) {
        return true;
    }

    location_snapshot_t old = {0};
    return location_snapshot_get(&old);
}

static void apply_sntp_time(void)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    time_snapshot_t snap = {
        .local_time = local,
        .unix_time = now,
        .from_rtc = false,
        .from_sntp = true,
    };
    time_snapshot_set(&snap);
    app_rtc_set_from_system();
    s_sntp_sync_applied = true;
    const int64_t elapsed_ms = s_sntp_started_us > 0
                                   ? (esp_timer_get_time() - s_sntp_started_us) / 1000
                                   : 0;
    APP_LOGI(TAG, "SNTP sync applied after %lld ms: %04d-%02d-%02d %02d:%02d:%02d",
             (long long)elapsed_ms,
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    runtime_event_log_append(RUNTIME_EVENT_SNTP_SYNC,
                             (int32_t)elapsed_ms,
                             (int32_t)now,
                             0);
    s_sntp_cycle_active = false;
    s_next_sntp_due_us = esp_timer_get_time() +
                         (int64_t)TIME_RESYNC_PERIOD_MS * 1000;
}

static esp_err_t begin_sntp_cycle(void)
{
    if (s_sntp_initialized) {
        esp_netif_sntp_deinit();
        s_sntp_initialized = false;
    }

    const char *primary_server = "time.cloudflare.com";
    const char *fallback_server = "time.google.com";
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(primary_server, fallback_server));
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "SNTP init failed; keep previous time");
        s_sntp_cycle_active = false;
        s_next_sntp_due_us = esp_timer_get_time() +
                             (int64_t)TIME_RESYNC_PERIOD_MS * 1000;
        return err;
    }

    s_sntp_initialized = true;
    s_sntp_cycle_active = true;
    s_sntp_sync_applied = false;
    s_sntp_fallback_logged = false;
    s_sntp_started_us = esp_timer_get_time();
    APP_LOGI(TAG,
             "SNTP request started: region=global primary=%s fallback=%s timeout=%d ms",
             primary_server, fallback_server, SNTP_SERVER_TIMEOUT_MS);
    return ESP_OK;
}

static void sntp_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_sntp_events, TIME_NET_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if ((bits & TIME_NET_EVENT) != 0) {
            /* SNTP 不依赖定位；拿到 IP 后立即启动，定位由独立任务并行处理。 */
            (void)begin_sntp_cycle();
        }

        const int64_t now_us = esp_timer_get_time();
        if (!s_sntp_cycle_active && s_next_sntp_due_us > 0 &&
            now_us >= s_next_sntp_due_us) {
            (void)begin_sntp_cycle();
        }

        if (s_sntp_cycle_active &&
            !s_sntp_fallback_logged && s_sntp_started_us > 0 &&
            now_us - s_sntp_started_us >=
                (int64_t)SNTP_SERVER_TIMEOUT_MS * 1000) {
            s_sntp_fallback_logged = true;
            APP_LOGW(TAG, "SNTP primary timeout window elapsed; waiting for fallback server");
        }

        if (s_sntp_cycle_active &&
            esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100)) == ESP_OK) {
            apply_sntp_time();
        } else if (s_sntp_cycle_active && s_sntp_started_us > 0 &&
                   now_us - s_sntp_started_us >=
                       (int64_t)SNTP_SYNC_ATTEMPT_TIMEOUT_MS * 1000) {
            APP_LOGW(TAG, "SNTP sync failed; keep previous time and retry in 4 hours");
            s_sntp_cycle_active = false;
            s_next_sntp_due_us = now_us + (int64_t)TIME_RESYNC_PERIOD_MS * 1000;
        }
    }
}

static void location_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_location_events, TIME_NET_EVENT, pdTRUE, pdFALSE,
                            portMAX_DELAY);
        location_snapshot_t cached = {0};
        const bool had_cached_location = location_snapshot_get(&cached);
        if (had_cached_location) {
            APP_LOGI(TAG, "request weather immediately from runtime location revision=%lu",
                     (unsigned long)cached.revision);
            app_weather_notify_network();
        }

        bool changed = false;
        if (refresh_location(&changed)) {
            if (!had_cached_location || changed) app_weather_notify_network();
        } else {
            APP_LOGW(TAG, "no valid location; weather request postponed");
        }
    }
}

esp_err_t app_time_init(void)
{
    esp_err_t err = app_time_prepare_timezone();
    if (err != ESP_OK) return err;
    if (location_snapshot_init() != ESP_OK) return ESP_ERR_NO_MEM;

    s_sntp_events = xEventGroupCreate();
    s_location_events = xEventGroupCreate();
    if (s_sntp_events == NULL || s_location_events == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sntp_task, "sntp_task", 4096, NULL, 4,
                    &s_sntp_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(location_task, "location_task", 4096, NULL, 3,
                       &s_location_task_handle) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

TaskHandle_t app_time_get_sntp_task_handle(void)
{
    return s_sntp_task_handle;
}

TaskHandle_t app_time_get_location_task_handle(void)
{
    return s_location_task_handle;
}

void app_time_notify_network(void)
{
    if (s_sntp_events != NULL) xEventGroupSetBits(s_sntp_events, TIME_NET_EVENT);
    if (s_location_events != NULL) xEventGroupSetBits(s_location_events, TIME_NET_EVENT);
}
