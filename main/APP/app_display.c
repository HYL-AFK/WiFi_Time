#include "app_display.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "common/app_log.h"
#include "common/runtime_event_log.h"
#include "common/time_snapshot.h"
#include "common/weather_snapshot.h"
#include "drivers/ws2812_matrix.h"

#define DISPLAY_REFRESH BIT0
#define DISPLAY_NETWORK_SUCCESS BIT1
#define DISPLAY_NETWORK_FAILURE BIT2
#define DISPLAY_ALL_EVENTS (DISPLAY_REFRESH | DISPLAY_NETWORK_SUCCESS | DISPLAY_NETWORK_FAILURE)

#define CLOCK_X 0
#define CLOCK_Y 0
#define CLOCK_W 13
#define CLOCK_H 5
#define CLOCK_HOUR_TENS_X 0
#define CLOCK_HOUR_ONES_X 3
#define CLOCK_COLON_X 6
#define CLOCK_MINUTE_TENS_X 7
#define CLOCK_MINUTE_ONES_X 10

#define WEATHER_X 0
#define WEATHER_Y 10 /* 天气图标占用左下角 5x5，最后一行 y=15 留空。 */
#define WEATHER_W 5
#define WEATHER_H 5
#define WEATHER_PLACEHOLDER_Y (WEATHER_Y + 2)

#define TEMP_TENS_X 7
#define TEMP_ONES_X 10
#define TEMP_Y 10

#define NEGATIVE_X 6
#define NEGATIVE_Y 12

#define DEGREE_X 13
#define DEGREE_Y 10
#define DEGREE_W 2
#define DEGREE_H 2

#define SECONDS_X 15
#define SECONDS_Y 0
#define SECONDS_H 15
#define DISPLAY_STATUS_LAMP_X 15
#define DISPLAY_STATUS_LAMP_Y 15

#define MATRIX_BRIGHTNESS_HIGH 60
#define MATRIX_BRIGHTNESS_LOW 20
#define DISPLAY_WHITE 70
#define DISPLAY_GREEN_R 8
#define DISPLAY_GREEN_G 90
#define DISPLAY_GREEN_B 28
#define DISPLAY_RED_R 100
#define DISPLAY_RED_G 8
#define DISPLAY_RED_B 8
#define DISPLAY_BLUE_R 8
#define DISPLAY_BLUE_G 30
#define DISPLAY_BLUE_B 100
#define DISPLAY_CYAN_R 0
#define DISPLAY_CYAN_G 95
#define DISPLAY_CYAN_B 95
#define DISPLAY_PINK_R 100
#define DISPLAY_PINK_G 20
#define DISPLAY_PINK_B 65
#define DISPLAY_NORMAL_R DISPLAY_GREEN_R
#define DISPLAY_NORMAL_G DISPLAY_GREEN_G
#define DISPLAY_NORMAL_B DISPLAY_GREEN_B
#define DISPLAY_REFRESH_PERIOD_MS 50
#define STARTUP_FRAME_PERIOD_MS 50
#define STARTUP_RING_TRAIL_LENGTH 5
#define STARTUP_RING_DIM 16
#define STARTUP_RING_LAP_DURATION_MS 6000
#define STARTUP_FADE_STEP_MS 100
#define BOOT_ANIMATION_CYCLE_COUNT 3
#define DISPLAY_NOTICE_DURATION_MS 1500
#define FLOW_FRAME_COUNT 32
#define FLOW_FRAME_DELAY_MS 25
#define WS2812_DUPLICATE_LATCH_US 300
#define WS2812_DIAGNOSTIC_INTERVAL 60
#define WS2812_KEEPALIVE_REFRESH_MS 250
#define DISPLAY_REFRESH_WARN_US 20000
#define DISPLAY_STATUS_BLINK_COUNT 2
#define DISPLAY_STATUS_BLINK_HALF_PERIOD_MS 250
#define DISPLAY_STATUS_BREATH_PERIOD_MS 1600
#define DISPLAY_SUMMARY_INTERVAL_MS 60000

static EventGroupHandle_t s_events;
static int64_t s_last_flow_minute = -1;
static uint32_t s_refresh_failures;
static uint32_t s_refresh_error_total;
static uint32_t s_non_grayscale_total;
static uint32_t s_duplicate_refresh_total;
static uint32_t s_main_frame_sequence;
static uint32_t s_last_main_crc;
static bool s_last_main_crc_valid;
static bool s_last_output_main;
static bool s_network_connected;
static bool s_network_seen;
static bool s_bluetooth_connected;
static bool s_weather_fetching;
static bool s_wifi_fault;
static bool s_weather_fault;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_blink_active;
static uint8_t s_blink_color;
static TickType_t s_blink_started;
static volatile uint8_t s_brightness_percent = MATRIX_BRIGHTNESS_HIGH;
static TaskHandle_t s_display_task_handle;
static TickType_t s_last_main_force_refresh;
static portMUX_TYPE s_health_lock = portMUX_INITIALIZER_UNLOCKED;
static app_display_health_t s_display_health;
static uint32_t s_next_animation_id;
static bool s_recovery_requested;
static const char *TAG = "app_display";

typedef enum {
    DISPLAY_MODE_BOOT = 0,
    DISPLAY_MODE_MAIN,
} display_mode_t;

typedef enum {
    NETWORK_NOTICE_NONE = 0,
    NETWORK_NOTICE_SUCCESS,
    NETWORK_NOTICE_FAILURE,
} network_notice_t;

typedef enum {
    STATUS_COLOR_CYAN = 0,
    STATUS_COLOR_BLUE,
} status_blink_color_t;

typedef struct {
    uint8_t x;
    uint8_t y;
} display_point_t;

typedef struct {
    bool active;
    bool abort_requested;
    uint32_t animation_id;
    uint8_t current_frame;
    uint8_t target_frame_count;
    TickType_t next_frame_tick;
    TickType_t started_at;
    uint32_t attempted_frames;
    uint32_t successful_frames;
    uint32_t max_refresh_ms;
} display_animation_state_t;

static display_animation_state_t s_animation;

static const display_point_t s_startup_clock_ring[] = {
    {6, 1}, {7, 1}, {8, 1}, {9, 1},
    {10, 2}, {11, 3}, {12, 4},
    {13, 5}, {13, 6}, {13, 7}, {13, 8}, {13, 9}, {13, 10},
    {12, 11}, {11, 12}, {10, 13},
    {9, 14}, {8, 14}, {7, 14}, {6, 14},
    {5, 13}, {4, 12}, {3, 11},
    {2, 10}, {2, 9}, {2, 8}, {2, 7}, {2, 6}, {2, 5},
    {3, 4}, {4, 3}, {5, 2},
};

static const uint8_t s_startup_trail_levels[STARTUP_RING_TRAIL_LENGTH] = {
    DISPLAY_WHITE, 54, 40, 29, 21,
};

static const display_point_t s_wifi_icon[] = {
    {3, 5}, {4, 4}, {5, 3}, {6, 3}, {7, 2}, {8, 2},
    {9, 3}, {10, 3}, {11, 4}, {12, 5},
    {5, 8}, {6, 7}, {7, 7}, {8, 7}, {9, 7}, {10, 8},
    {7, 11}, {8, 11},
};

/* 2×5 字模为相邻数字留出空列，避免时间和温度字形粘连。 */
/* 时间和温度使用标准 3x5 数字，保证小尺寸下仍可辨认。 */
static const uint8_t s_digit_3x5[10][5] = {
    {0x07, 0x05, 0x05, 0x05, 0x07},
    {0x02, 0x02, 0x02, 0x02, 0x02},
    {0x07, 0x01, 0x07, 0x04, 0x07},
    {0x07, 0x01, 0x07, 0x01, 0x07},
    {0x05, 0x05, 0x07, 0x01, 0x01},
    {0x07, 0x04, 0x07, 0x01, 0x07},
    {0x07, 0x04, 0x07, 0x05, 0x07},
    {0x07, 0x01, 0x01, 0x01, 0x01},
    {0x07, 0x05, 0x07, 0x05, 0x07},
    {0x07, 0x05, 0x07, 0x01, 0x07},
};

/* 每个天气图标都是 5x5 位图，最高位对应图标最左列。 */
static const uint8_t s_weather_clear[WEATHER_H] = {0x04, 0x11, 0x0e, 0x11, 0x04}; /* 晴天 */
static const uint8_t s_weather_cloud[WEATHER_H] = {0x00, 0x0e, 0x1f, 0x1f, 0x00}; /* 多云/雾 */
static const uint8_t s_weather_rain[WEATHER_H] = {0x04, 0x0e, 0x1f, 0x0a, 0x15}; /* 降雨 */
static const uint8_t s_weather_snow[WEATHER_H] = {0x04, 0x15, 0x0e, 0x15, 0x04}; /* 降雪 */
static const uint8_t s_weather_thunder[WEATHER_H] = {0x04, 0x0e, 0x1f, 0x04, 0x08}; /* 雷雨 */

static void set_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height,
                     uint8_t r, uint8_t g, uint8_t b)
{
    for (uint8_t row = 0; row < height; ++row) {
        for (uint8_t col = 0; col < width; ++col) {
            if (x + col < MATRIX_WIDTH && y + row < MATRIX_HEIGHT) {
                ws2812_matrix_set_pixel(x + col, y + row, r, g, b);
            }
        }
    }
}

static void draw_points(const display_point_t *points, size_t count,
                        uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0; i < count; ++i) {
        ws2812_matrix_set_pixel(points[i].x, points[i].y, r, g, b);
    }
}

static void draw_digit_3x5(uint8_t x, uint8_t y, uint8_t digit,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if (digit > 9) return;
    for (uint8_t row = 0; row < 5; ++row) {
        for (uint8_t col = 0; col < 3; ++col) {
            if (s_digit_3x5[digit][row] & (1u << (2u - col))) {
                ws2812_matrix_set_pixel(x + col, y + row, r, g, b);
            }
        }
    }
}

static void draw_clock(const struct tm *local_time)
{
    const uint8_t hour = (uint8_t)local_time->tm_hour;
    const uint8_t minute = (uint8_t)local_time->tm_min;
    const uint8_t r = DISPLAY_WHITE;
    const uint8_t g = DISPLAY_WHITE;
    const uint8_t b = DISPLAY_WHITE;

    draw_digit_3x5(CLOCK_HOUR_TENS_X, CLOCK_Y, hour / 10, r, g, b);
    draw_digit_3x5(CLOCK_HOUR_ONES_X, CLOCK_Y, hour % 10, r, g, b);
    if ((local_time->tm_sec & 1) == 0) {
        ws2812_matrix_set_pixel(CLOCK_COLON_X, CLOCK_Y + 1, r, g, b);
        ws2812_matrix_set_pixel(CLOCK_COLON_X, CLOCK_Y + 3, r, g, b);
    }
    draw_digit_3x5(CLOCK_MINUTE_TENS_X, CLOCK_Y, minute / 10, r, g, b);
    draw_digit_3x5(CLOCK_MINUTE_ONES_X, CLOCK_Y, minute % 10, r, g, b);
}

static const uint8_t *weather_glyph(int weather_code)
{
    if (weather_code == 0) return s_weather_clear;
    if ((weather_code >= 1 && weather_code <= 3) ||
        weather_code == 45 || weather_code == 48) return s_weather_cloud;
    if ((weather_code >= 51 && weather_code <= 67) ||
        (weather_code >= 80 && weather_code <= 82)) return s_weather_rain;
    if ((weather_code >= 71 && weather_code <= 77) ||
        (weather_code >= 85 && weather_code <= 86)) return s_weather_snow;
    if (weather_code >= 95 && weather_code <= 99) return s_weather_thunder;
    return s_weather_cloud;
}

static void draw_weather_placeholder(void)
{
    set_rect(WEATHER_X, WEATHER_PLACEHOLDER_Y, 2, 1,
             DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    set_rect(WEATHER_X + 3, WEATHER_PLACEHOLDER_Y, 2, 1,
             DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
}

static void draw_weather(const weather_snapshot_t *weather)
{
    if (!weather->valid) {
        draw_weather_placeholder();
        return;
    }
    const uint8_t *glyph = weather_glyph(weather->weather_code);
    for (uint8_t row = 0; row < WEATHER_H; ++row) {
        for (uint8_t col = 0; col < WEATHER_W; ++col) {
            if (glyph[row] & (1u << (WEATHER_W - 1u - col))) {
                ws2812_matrix_set_pixel(WEATHER_X + col, WEATHER_Y + row,
                                        DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
            }
        }
    }
}

static void draw_temperature(const weather_snapshot_t *weather)
{
    if (weather->network_error) {
        for (uint8_t i = 0; i < 5; ++i) {
            ws2812_matrix_set_pixel(TEMP_TENS_X + i, TEMP_Y + i,
                                    DISPLAY_RED_R, DISPLAY_RED_G, DISPLAY_RED_B);
            ws2812_matrix_set_pixel(TEMP_TENS_X + 4u - i, TEMP_Y + i,
                                    DISPLAY_RED_R, DISPLAY_RED_G, DISPLAY_RED_B);
        }
        return;
    }
    if (!weather->temperature_valid) return;

    int temperature = (int)(weather->temperature_c >= 0.0f ?
                            weather->temperature_c + 0.5f :
                            weather->temperature_c - 0.5f);
    const bool negative = temperature < 0;
    if (negative) temperature = -temperature;
    if (temperature > 99) temperature = 99;

    if (negative) {
        ws2812_matrix_set_pixel(NEGATIVE_X, NEGATIVE_Y,
                                DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
    draw_digit_3x5(TEMP_TENS_X, TEMP_Y, (uint8_t)(temperature / 10),
                   DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    draw_digit_3x5(TEMP_ONES_X, TEMP_Y, (uint8_t)(temperature % 10),
                   DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);

    /* 度号属于温度显示，即使尚未联网使用 RTC 温度时也必须显示。 */
    set_rect(DEGREE_X, DEGREE_Y, DEGREE_W, DEGREE_H,
             DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
}

static void draw_seconds(uint8_t seconds)
{
    const uint8_t cycle = (uint8_t)(seconds / SECONDS_H);
    const uint8_t step = (uint8_t)(seconds % SECONDS_H);
    const uint8_t lit = (cycle & 1u) == 0u
                            ? (uint8_t)(step + 1u)
                            : (uint8_t)(SECONDS_H - step - 1u);
    for (uint8_t row = 0; row < lit; ++row) {
        const uint8_t y = (cycle & 1u) == 0u
                              ? (uint8_t)(SECONDS_Y + SECONDS_H - 1u - row)
                              : (uint8_t)(SECONDS_Y + row);
        ws2812_matrix_set_pixel(SECONDS_X, y, DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
}

static uint8_t status_breath_level(TickType_t now)
{
    const uint32_t phase =
        (uint32_t)((now * portTICK_PERIOD_MS) % DISPLAY_STATUS_BREATH_PERIOD_MS);
    const uint32_t half = DISPLAY_STATUS_BREATH_PERIOD_MS / 2U;
    const uint32_t distance = phase <= half ? phase : DISPLAY_STATUS_BREATH_PERIOD_MS - phase;
    return (uint8_t)(20U + (distance * 80U) / half);
}

static void draw_status_lamp(void)
{
    const TickType_t now = xTaskGetTickCount();
    bool wifi_fault;
    bool weather_fault;
    bool bluetooth_connected;
    bool weather_fetching;
    bool blink_active;
    uint8_t blink_color;
    TickType_t blink_started;
    portENTER_CRITICAL(&s_status_lock);
    wifi_fault = s_wifi_fault;
    weather_fault = s_weather_fault;
    bluetooth_connected = s_bluetooth_connected;
    weather_fetching = s_weather_fetching;
    blink_active = s_blink_active;
    blink_color = s_blink_color;
    blink_started = s_blink_started;
    portEXIT_CRITICAL(&s_status_lock);

    uint8_t red = DISPLAY_NORMAL_R;
    uint8_t green = DISPLAY_NORMAL_G;
    uint8_t blue = DISPLAY_NORMAL_B;
    uint8_t level = status_breath_level(now);

    bool render_blink = false;
    if (!wifi_fault && !weather_fault && blink_active) {
        const uint32_t elapsed_ms = (uint32_t)((now - blink_started) * portTICK_PERIOD_MS);
        const uint32_t phase = elapsed_ms / DISPLAY_STATUS_BLINK_HALF_PERIOD_MS;
        if (phase >= DISPLAY_STATUS_BLINK_COUNT * 2U) {
            portENTER_CRITICAL(&s_status_lock);
            if (s_blink_active && s_blink_started == blink_started) {
                s_blink_active = false;
            }
            portEXIT_CRITICAL(&s_status_lock);
        } else {
            render_blink = true;
            if ((phase & 1U) == 0U) {
                if (blink_color == STATUS_COLOR_BLUE) {
                    red = DISPLAY_BLUE_R; green = DISPLAY_BLUE_G; blue = DISPLAY_BLUE_B;
                } else {
                    red = DISPLAY_CYAN_R; green = DISPLAY_CYAN_G; blue = DISPLAY_CYAN_B;
                }
            } else {
                level = 0;
            }
        }
    }

    if (wifi_fault || weather_fault) {
        red = DISPLAY_RED_R; green = DISPLAY_RED_G; blue = DISPLAY_RED_B;
    } else if (render_blink) {
        /* The temporary blink owns this frame; the next frame returns to the base state. */
    } else if (bluetooth_connected) {
        red = DISPLAY_BLUE_R; green = DISPLAY_BLUE_G; blue = DISPLAY_BLUE_B;
    } else if (weather_fetching) {
        red = DISPLAY_CYAN_R; green = DISPLAY_CYAN_G; blue = DISPLAY_CYAN_B;
    } else {
        red = DISPLAY_NORMAL_R; green = DISPLAY_NORMAL_G; blue = DISPLAY_NORMAL_B;
    }

    ws2812_matrix_set_pixel(DISPLAY_STATUS_LAMP_X, DISPLAY_STATUS_LAMP_Y,
                            (uint8_t)((red * level) / 100U),
                            (uint8_t)((green * level) / 100U),
                            (uint8_t)((blue * level) / 100U));
}

static void display_record_event(runtime_event_type_t type, int32_t value1,
                                 int32_t value2, uint32_t flags)
{
    (void)runtime_event_log_append(type, value1, value2, flags);
}

static void display_update_health(bool refresh_ok)
{
    static uint32_t recovery_seen;
    static uint32_t error_seen;
    ws2812_refresh_diagnostics_t diag = {0};
    ws2812_matrix_get_refresh_diagnostics(&diag);
    const bool new_error = diag.error_count != error_seen;
    error_seen = diag.error_count;
    if (diag.recovery_count != recovery_seen) {
        recovery_seen = diag.recovery_count;
        display_record_event(RUNTIME_EVENT_DISPLAY_ERROR,
                             (int32_t)s_animation.current_frame,
                             diag.last_error,
                             refresh_ok ? DISPLAY_RMT_RECOVERY_SUCCESS
                                         : DISPLAY_RMT_RECOVERY_FAILURE);
    }
    portENTER_CRITICAL(&s_health_lock);
    s_display_health.heartbeat++;
    s_display_health.last_progress_tick = xTaskGetTickCount();
    s_display_health.last_refresh_tick = s_display_health.last_progress_tick;
    s_display_health.last_refresh_ms = diag.last_refresh_ms;
    s_display_health.animation_id = s_animation.animation_id;
    s_display_health.animation_frame = s_animation.current_frame;
    s_display_health.animation_active = s_animation.active;
    s_display_health.refresh_errors = diag.error_count;
    s_display_health.recovery_count = diag.recovery_count;
    portEXIT_CRITICAL(&s_health_lock);

    if (!refresh_ok && new_error) {
        display_record_event(RUNTIME_EVENT_DISPLAY_ERROR,
                             (int32_t)s_animation.current_frame,
                             diag.last_error,
                             diag.last_error == ESP_ERR_TIMEOUT
                                 ? DISPLAY_REFRESH_TIMEOUT : 0);
    }
}

static void display_heartbeat(void)
{
    portENTER_CRITICAL(&s_health_lock);
    s_display_health.heartbeat++;
    s_display_health.last_progress_tick = xTaskGetTickCount();
    s_display_health.animation_id = s_animation.animation_id;
    s_display_health.animation_frame = s_animation.current_frame;
    s_display_health.animation_active = s_animation.active;
    portEXIT_CRITICAL(&s_health_lock);
}

static void display_summary_if_due(TickType_t now)
{
    static TickType_t last_summary;
    if ((TickType_t)(now - last_summary) < pdMS_TO_TICKS(DISPLAY_SUMMARY_INTERVAL_MS)) {
        return;
    }
    last_summary = now;
    app_display_health_t health;
    app_display_get_health(&health);
    display_record_event(RUNTIME_EVENT_DISPLAY_ERROR,
                         (int32_t)health.animation_frame,
                         (int32_t)health.last_refresh_ms,
                         DISPLAY_SUMMARY |
                             ((health.refresh_errors & 0xffU) << 16) |
                             ((health.recovery_count & 0xffU) << 24));
}

/* WS2812 发送失败时只重试一次，避免短暂总线错误留下半帧数据。 */
static esp_err_t show_frame_with_retry(void)
{
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = ws2812_matrix_show(s_brightness_percent);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us > DISPLAY_REFRESH_WARN_US) {
        APP_LOGW(TAG, "WS2812 refresh slow: elapsed_us=%" PRId64 " err=%s",
                 elapsed_us, esp_err_to_name(err));
    }
    display_update_health(err == ESP_OK);
    if (err == ESP_OK) {
        if (s_refresh_failures > 0U) {
            APP_LOGI(TAG, "WS2812 refresh recovered after %" PRIu32 " failed frames",
                     s_refresh_failures);
        }
        s_refresh_failures = 0;
        return ESP_OK;
    }

    APP_LOG_ERR(TAG, err, "WS2812 refresh failed, retrying, consecutive=%" PRIu32,
                s_refresh_failures + 1U);
    s_refresh_error_total++;
    vTaskDelay(pdMS_TO_TICKS(1));

    err = ws2812_matrix_show(s_brightness_percent);
    display_update_health(err == ESP_OK);
    if (err == ESP_OK) {
        APP_LOGW(TAG, "WS2812 refresh recovered after retry");
        s_refresh_failures = 0;
        return ESP_OK;
    }

    s_refresh_failures++;
    s_refresh_error_total++;
    APP_LOG_ERR(TAG, err, "WS2812 refresh retry failed, consecutive=%" PRIu32,
                s_refresh_failures);
    return err;
}

static esp_err_t show_frame_force_with_retry(void)
{
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = ws2812_matrix_show_force(s_brightness_percent);
    display_update_health(err == ESP_OK);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us > DISPLAY_REFRESH_WARN_US) {
        APP_LOGW(TAG, "WS2812 forced refresh slow: elapsed_us=%" PRId64 " err=%s",
                 elapsed_us, esp_err_to_name(err));
    }
    if (err == ESP_OK) return ESP_OK;

    s_refresh_error_total++;
    APP_LOG_ERR(TAG, err, "WS2812 forced refresh failed, retrying, total=%" PRIu32,
                s_refresh_error_total);
    vTaskDelay(pdMS_TO_TICKS(1));
    err = ws2812_matrix_show_force(s_brightness_percent);
    display_update_health(err == ESP_OK);
    if (err != ESP_OK) {
        s_refresh_error_total++;
        APP_LOG_ERR(TAG, err, "WS2812 forced refresh retry failed, total=%" PRIu32,
                    s_refresh_error_total);
    }
    return err;
}

static uint8_t scale_startup_level(uint8_t level, uint8_t scale_percent)
{
    return (uint8_t)(((uint16_t)level * scale_percent) / 100U);
}

static uint32_t startup_frame_for_elapsed(uint32_t elapsed_ms)
{
    const uint32_t ring_count =
        (uint32_t)(sizeof(s_startup_clock_ring) / sizeof(s_startup_clock_ring[0]));
    return (uint32_t)(((uint64_t)elapsed_ms * ring_count) /
                      STARTUP_RING_LAP_DURATION_MS);
}

static void render_startup_frame_scaled(uint32_t frame, uint8_t scale_percent)
{
    s_last_output_main = false;
    ws2812_matrix_clear();

    const uint8_t ring_dim = scale_startup_level(STARTUP_RING_DIM, scale_percent);
    draw_points(s_startup_clock_ring,
                sizeof(s_startup_clock_ring) / sizeof(s_startup_clock_ring[0]),
                ring_dim, ring_dim, ring_dim);

    const size_t ring_count =
        sizeof(s_startup_clock_ring) / sizeof(s_startup_clock_ring[0]);
    const size_t head = frame % ring_count;
    for (size_t trail = STARTUP_RING_TRAIL_LENGTH; trail > 0; --trail) {
        const size_t offset = trail - 1U;
        const size_t index = (head + ring_count - offset) % ring_count;
        const uint8_t level =
            scale_startup_level(s_startup_trail_levels[offset], scale_percent);
        const display_point_t point = s_startup_clock_ring[index];
        ws2812_matrix_set_pixel(point.x, point.y, level, level, level);
    }

    (void)show_frame_with_retry();
}

static void render_startup_frame(uint32_t frame)
{
    render_startup_frame_scaled(frame, 100);
}

static void render_startup_fade_out(uint32_t startup_frame)
{
    render_startup_frame_scaled(startup_frame, 67);
    vTaskDelay(pdMS_TO_TICKS(STARTUP_FADE_STEP_MS));
    render_startup_frame_scaled(startup_frame, 33);
    vTaskDelay(pdMS_TO_TICKS(STARTUP_FADE_STEP_MS));
    ws2812_matrix_clear();
    (void)show_frame_with_retry();
}

static void render_network_success(void)
{
    static const display_point_t check[] = {
        {10, 12}, {11, 13}, {12, 14}, {13, 13}, {14, 12}, {15, 11},
    };

    s_last_output_main = false;
    ws2812_matrix_clear();
    draw_points(s_wifi_icon, sizeof(s_wifi_icon) / sizeof(s_wifi_icon[0]),
                DISPLAY_GREEN_R, DISPLAY_GREEN_G, DISPLAY_GREEN_B);
    draw_points(check, sizeof(check) / sizeof(check[0]),
                DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    (void)show_frame_with_retry();
}

static void render_network_failure(void)
{
    static const display_point_t cross[] = {
        {11, 11}, {12, 12}, {13, 13}, {14, 14}, {15, 15},
        {15, 11}, {14, 12}, {12, 14}, {11, 15},
    };

    s_last_output_main = false;
    ws2812_matrix_clear();
    draw_points(s_wifi_icon, sizeof(s_wifi_icon) / sizeof(s_wifi_icon[0]),
                DISPLAY_RED_R, DISPLAY_RED_G, DISPLAY_RED_B);
    draw_points(cross, sizeof(cross) / sizeof(cross[0]),
                DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    (void)show_frame_with_retry();
}

static void set_elapsed_time(struct tm *out, TickType_t display_started)
{
    if (out == NULL) return;

    *out = (struct tm){0};
    const uint32_t elapsed_seconds =
        (uint32_t)(((xTaskGetTickCount() - display_started) * portTICK_PERIOD_MS) / 1000U);
    out->tm_hour = (int)((elapsed_seconds / 3600U) % 24U);
    out->tm_min = (int)((elapsed_seconds / 60U) % 60U);
    out->tm_sec = (int)(elapsed_seconds % 60U);
}

static bool render_normal(TickType_t display_started, time_snapshot_t *out_snapshot)
{
    time_snapshot_t time_snapshot = {0};
    weather_snapshot_t weather = {0};
    const bool time_ready = time_snapshot_is_ready();
    if (time_ready) {
        time_snapshot_get(&time_snapshot);
    } else {
        set_elapsed_time(&time_snapshot.local_time, display_started);
    }
    weather_snapshot_get(&weather);

    if (time_ready) {
        time_t now = time(NULL);
        struct tm local_time;
        if (now != (time_t)-1) {
            localtime_r(&now, &local_time);
            time_snapshot.local_time = local_time;
            time_snapshot.unix_time = now;
        }
    }

    if (out_snapshot != NULL) {
        *out_snapshot = time_snapshot;
    }

    ws2812_matrix_clear();
    draw_clock(&time_snapshot.local_time);
    draw_weather(&weather);
    draw_temperature(&weather);
    draw_seconds((uint8_t)time_snapshot.local_time.tm_sec);
    draw_status_lamp();

    ws2812_frame_diagnostics_t diag = {0};
    ws2812_matrix_get_frame_diagnostics(&diag);
    if (!diag.grayscale &&
        !(diag.x == DISPLAY_STATUS_LAMP_X && diag.y == DISPLAY_STATUS_LAMP_Y)) {
        s_main_frame_sequence++;
        s_non_grayscale_total++;
        APP_LOGE(TAG,
                 "drop non-grayscale main frame: frame=%" PRIu32 " crc32=%08" PRIx32
                 " x=%u y=%u physical=%u rgb=(%u,%u,%u)",
                 s_main_frame_sequence, diag.crc32, diag.x, diag.y,
                 diag.physical_index, diag.red, diag.green, diag.blue);
        return time_ready;
    }

    const bool frame_changed = !s_last_output_main || !s_last_main_crc_valid ||
                               diag.crc32 != s_last_main_crc;
    if (!frame_changed) {
        const TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - s_last_main_force_refresh) >=
            pdMS_TO_TICKS(WS2812_KEEPALIVE_REFRESH_MS)) {
            if (show_frame_force_with_retry() == ESP_OK) {
                s_duplicate_refresh_total++;
                s_last_main_force_refresh = now;
            } else {
                s_last_output_main = false;
            }
        }
        return time_ready;
    }

    s_main_frame_sequence++;
    if (show_frame_with_retry() != ESP_OK) {
        s_last_output_main = false;
        return time_ready;
    }

    esp_rom_delay_us(WS2812_DUPLICATE_LATCH_US);
    if (show_frame_force_with_retry() != ESP_OK) {
        s_last_output_main = false;
        return time_ready;
    }

    s_duplicate_refresh_total++;
    s_last_main_crc = diag.crc32;
    s_last_main_crc_valid = true;
    s_last_output_main = true;
    s_last_main_force_refresh = xTaskGetTickCount();
    if ((s_main_frame_sequence % WS2812_DIAGNOSTIC_INTERVAL) == 0U) {
        APP_LOGI(TAG,
                 "WS2812 diagnostics: main_frames=%" PRIu32 " crc32=%08" PRIx32
                 " non_gray=%" PRIu32 " tx_errors=%" PRIu32
                 " duplicate_refreshes=%" PRIu32,
                 s_main_frame_sequence, diag.crc32, s_non_grayscale_total,
                 s_refresh_error_total, s_duplicate_refresh_total);
    }

    return time_ready;
}

static bool flow_animation_due(const time_snapshot_t *snapshot)
{
    if (snapshot->local_time.tm_sec != 0) return false;
    if (snapshot->local_time.tm_min != 0 && snapshot->local_time.tm_min != 30) return false;

    const int64_t minute = (int64_t)(snapshot->unix_time / 60);
    if (minute == s_last_flow_minute) return false;
    s_last_flow_minute = minute;
    return true;
}

static void render_flow_frame(uint8_t phase)
{
    s_last_output_main = false;
    ws2812_matrix_clear();
    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
            const uint8_t band = (uint8_t)((x + y + phase) & 0x0f);
            if (band < 4) {
                const uint8_t level = (uint8_t)(4 - band);
                const uint8_t intensity = (uint8_t)((level * DISPLAY_WHITE) / 4u);
                ws2812_matrix_set_pixel(x, y, intensity, intensity, intensity);
            }
        }
    }
}

static void flow_animation_begin(void)
{
    if (s_animation.active) return;
    memset(&s_animation, 0, sizeof(s_animation));
    s_animation.active = true;
    s_animation.animation_id = ++s_next_animation_id;
    s_animation.target_frame_count = FLOW_FRAME_COUNT;
    s_animation.started_at = xTaskGetTickCount();
    s_animation.next_frame_tick = s_animation.started_at;
    display_record_event(RUNTIME_EVENT_DISPLAY_ANIMATION,
                         0, (int32_t)s_animation.animation_id,
                         DISPLAY_ANIMATION_BEGIN);
}

static void flow_animation_finish(bool aborted)
{
    if (!s_animation.active) return;
    const TickType_t now = xTaskGetTickCount();
    const uint32_t elapsed_ms = (uint32_t)((now - s_animation.started_at) *
                                           portTICK_PERIOD_MS);
    const uint32_t phase = aborted ? DISPLAY_ANIMATION_ABORT : DISPLAY_ANIMATION_COMPLETE;
    const uint32_t summary_flags =
        phase |
        ((s_animation.successful_frames & 0xffU) << DISPLAY_ANIMATION_SUCCESS_SHIFT) |
        ((s_animation.max_refresh_ms & 0xffU) << DISPLAY_ANIMATION_MAX_REFRESH_SHIFT);
    display_record_event(RUNTIME_EVENT_DISPLAY_ANIMATION,
                         (int32_t)s_animation.attempted_frames,
                         (int32_t)elapsed_ms,
                         summary_flags);
    s_animation.active = false;
    s_animation.abort_requested = false;
    s_last_output_main = false;
    /* Always leave the animation with a complete, normal frame. */
    (void)render_normal(xTaskGetTickCount(), NULL);
}

static void flow_animation_step(void)
{
    if (!s_animation.active) return;
    const TickType_t now = xTaskGetTickCount();
    if (s_animation.abort_requested) {
        flow_animation_finish(true);
        return;
    }
    if ((int32_t)(now - s_animation.next_frame_tick) < 0) return;

    render_flow_frame(s_animation.current_frame);
    s_animation.attempted_frames++;
    const esp_err_t err = show_frame_with_retry();
    ws2812_refresh_diagnostics_t diag = {0};
    ws2812_matrix_get_refresh_diagnostics(&diag);
    if (diag.last_refresh_ms > s_animation.max_refresh_ms) {
        s_animation.max_refresh_ms = diag.last_refresh_ms;
    }
    if (err != ESP_OK) {
        s_animation.abort_requested = true;
        display_record_event(RUNTIME_EVENT_DISPLAY_ERROR,
                             (int32_t)s_animation.current_frame,
                             err,
                             err == ESP_ERR_TIMEOUT ? DISPLAY_REFRESH_TIMEOUT : 0);
        flow_animation_finish(true);
        return;
    }

    s_animation.successful_frames++;
    if (++s_animation.current_frame >= s_animation.target_frame_count) {
        flow_animation_finish(false);
    } else {
        s_animation.next_frame_tick = now + pdMS_TO_TICKS(FLOW_FRAME_DELAY_MS);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    const TickType_t boot_started = xTaskGetTickCount();
    const TickType_t frame_period = pdMS_TO_TICKS(STARTUP_FRAME_PERIOD_MS);
    TickType_t next_frame = boot_started;
    TickType_t cycle_started = boot_started;
    uint32_t startup_frame = 0;
    uint32_t boot_animation_cycle = 0;
    display_mode_t mode = DISPLAY_MODE_BOOT;
    network_notice_t pending_notice = NETWORK_NOTICE_NONE;

    for (;;) {
        const TickType_t wait_ticks = s_animation.active
                                          ? pdMS_TO_TICKS(FLOW_FRAME_DELAY_MS)
                                          : pdMS_TO_TICKS(DISPLAY_REFRESH_PERIOD_MS);
        const EventBits_t events =
            xEventGroupWaitBits(s_events, DISPLAY_ALL_EVENTS, pdTRUE, pdFALSE,
                                wait_ticks);
        display_heartbeat();
        display_summary_if_due(xTaskGetTickCount());

        portENTER_CRITICAL(&s_health_lock);
        const bool recovery_requested = s_recovery_requested;
        s_recovery_requested = false;
        portEXIT_CRITICAL(&s_health_lock);
        if (recovery_requested) s_animation.abort_requested = true;

        if (mode == DISPLAY_MODE_BOOT) {
            if ((events & DISPLAY_NETWORK_SUCCESS) != 0) {
                pending_notice = NETWORK_NOTICE_SUCCESS;
            } else if ((events & DISPLAY_NETWORK_FAILURE) != 0) {
                pending_notice = NETWORK_NOTICE_FAILURE;
            }
        }

        if (mode == DISPLAY_MODE_BOOT) {
            const TickType_t now = xTaskGetTickCount();
            const uint32_t cycle_elapsed_ms =
                (uint32_t)((now - cycle_started) * portTICK_PERIOD_MS);
            if ((int32_t)(now - next_frame) >= 0) {
                startup_frame = startup_frame_for_elapsed(cycle_elapsed_ms);
                render_startup_frame(startup_frame);
                next_frame = now + frame_period;
            }

            if (cycle_elapsed_ms < STARTUP_RING_LAP_DURATION_MS) continue;

            cycle_started += pdMS_TO_TICKS(STARTUP_RING_LAP_DURATION_MS);
            boot_animation_cycle++;
            const bool time_ready = time_snapshot_is_ready();
            const bool boot_timed_out_without_time_source =
                !time_ready && boot_animation_cycle >= BOOT_ANIMATION_CYCLE_COUNT;
            const bool boot_ready_for_main = time_ready || s_network_seen ||
                                             boot_timed_out_without_time_source;

            if (!boot_ready_for_main) {
                continue;
            }

            render_startup_fade_out(startup_frame);
            if (pending_notice == NETWORK_NOTICE_SUCCESS || s_network_connected) {
                render_network_success();
                vTaskDelay(pdMS_TO_TICKS(DISPLAY_NOTICE_DURATION_MS));
            } else if (pending_notice == NETWORK_NOTICE_FAILURE ||
                       boot_timed_out_without_time_source) {
                render_network_failure();
                vTaskDelay(pdMS_TO_TICKS(DISPLAY_NOTICE_DURATION_MS));
            }
            pending_notice = NETWORK_NOTICE_NONE;
            next_frame = xTaskGetTickCount();
            mode = DISPLAY_MODE_MAIN;
            continue;
        }

        if (s_animation.active) {
            flow_animation_step();
            continue;
        }

        time_snapshot_t snapshot = {0};
        const bool time_ready = render_normal(boot_started, &snapshot);
        if (time_ready && flow_animation_due(&snapshot)) {
            flow_animation_begin();
            flow_animation_step();
        }
    }
}

/* 初始化 WS2812、事件组，并创建唯一拥有显示外设的 FreeRTOS 任务。 */
esp_err_t app_display_init(bool network_expected)
{
    (void)network_expected;
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    esp_err_t err = ws2812_matrix_init();
    if (err != ESP_OK) return err;

    APP_LOGI(TAG, "WS2812 config: gpio=10 dma=1 brightness=%u pixels=%u",
             s_brightness_percent, MATRIX_PIXELS);

    /* 先发送全黑帧，清除灯珠上电后的不确定状态。 */
    ws2812_matrix_clear();
    err = show_frame_with_retry();
    if (err != ESP_OK) return err;

    if (xTaskCreate(display_task, "display_task", 4096, NULL, 6,
                    &s_display_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_health_lock);
    s_display_health.last_progress_tick = xTaskGetTickCount();
    s_display_health.last_refresh_tick = s_display_health.last_progress_tick;
    portEXIT_CRITICAL(&s_health_lock);
    xEventGroupSetBits(s_events, DISPLAY_REFRESH);
    return ESP_OK;
}

TaskHandle_t app_display_get_task_handle(void)
{
    return s_display_task_handle;
}

void app_display_get_health(app_display_health_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_health_lock);
    *out = s_display_health;
    portEXIT_CRITICAL(&s_health_lock);
}

void app_display_request_recovery(void)
{
    portENTER_CRITICAL(&s_health_lock);
    s_recovery_requested = true;
    portEXIT_CRITICAL(&s_health_lock);
    app_display_request_refresh();
}

/* 其他任务只发送刷新事件，不直接操作 WS2812。 */
void app_display_request_refresh(void)
{
    if (s_events) xEventGroupSetBits(s_events, DISPLAY_REFRESH);
}

void app_display_set_brightness(uint8_t brightness_percent)
{
    if (brightness_percent > 100U) brightness_percent = 100U;
    if (s_brightness_percent == brightness_percent) return;
    s_brightness_percent = brightness_percent;
    app_display_request_refresh();
}

void app_display_set_network_connected(bool connected)
{
    portENTER_CRITICAL(&s_status_lock);
    s_network_connected = connected;
    if (connected) s_network_seen = true;
    portEXIT_CRITICAL(&s_status_lock);
    app_display_request_refresh();
}

void app_display_set_wifi_fault(bool fault)
{
    portENTER_CRITICAL(&s_status_lock);
    s_wifi_fault = fault;
    portEXIT_CRITICAL(&s_status_lock);
    app_display_request_refresh();
}

void app_display_set_weather_fault(bool fault)
{
    portENTER_CRITICAL(&s_status_lock);
    s_weather_fault = fault;
    portEXIT_CRITICAL(&s_status_lock);
    app_display_request_refresh();
}

void app_display_set_bluetooth_connected(bool connected)
{
    portENTER_CRITICAL(&s_status_lock);
    s_bluetooth_connected = connected;
    portEXIT_CRITICAL(&s_status_lock);
    app_display_request_refresh();
}

void app_display_set_weather_fetching(bool fetching)
{
    portENTER_CRITICAL(&s_status_lock);
    s_weather_fetching = fetching;
    portEXIT_CRITICAL(&s_status_lock);
    app_display_request_refresh();
}

static void start_status_blink(status_blink_color_t color)
{
    portENTER_CRITICAL(&s_status_lock);
    s_blink_color = (uint8_t)color;
    s_blink_started = xTaskGetTickCount();
    s_blink_active = true;
    portEXIT_CRITICAL(&s_status_lock);
    app_display_request_refresh();
}

void app_display_show_bluetooth_connected(void)
{
    start_status_blink(STATUS_COLOR_BLUE);
}

void app_display_show_network_success(void)
{
    if (!s_events) return;
    xEventGroupClearBits(s_events, DISPLAY_NETWORK_FAILURE);
    xEventGroupSetBits(s_events, DISPLAY_NETWORK_SUCCESS);
}

void app_display_show_network_failure(void)
{
    if (!s_events) return;
    xEventGroupClearBits(s_events, DISPLAY_NETWORK_SUCCESS);
    xEventGroupSetBits(s_events, DISPLAY_NETWORK_FAILURE);
}

void app_display_show_weather_refresh_success(void)
{
    start_status_blink(STATUS_COLOR_CYAN);
}

void app_display_show_weather_refresh_failure(void)
{
    app_display_request_refresh();
}
