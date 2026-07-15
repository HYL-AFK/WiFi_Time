#include "app_display.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "common/app_log.h"
#include "common/time_snapshot.h"
#include "common/weather_snapshot.h"
#include "drivers/ws2812_matrix.h"

#define DISPLAY_REFRESH BIT0

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

#define TEMP_TENS_X 7
#define TEMP_ONES_X 10
#define TEMP_Y 10

#define NEGATIVE_X 6
#define NEGATIVE_Y 12

#define DEGREE_X 13
#define DEGREE_Y 9
#define DEGREE_W 2
#define DEGREE_H 2

#define SECONDS_X 15
#define SECONDS_Y 0
#define SECONDS_H 15

#define MATRIX_BRIGHTNESS 35
#define DISPLAY_WHITE 70
#define DISPLAY_REFRESH_PERIOD_MS 50
#define FLOW_FRAME_COUNT 32
#define FLOW_FRAME_DELAY_MS 25

static EventGroupHandle_t s_events;
static int64_t s_last_flow_minute = -1;
static const char *TAG = "app_display";

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
    ws2812_matrix_set_pixel(CLOCK_COLON_X, CLOCK_Y + 1, r, g, b);
    ws2812_matrix_set_pixel(CLOCK_COLON_X, CLOCK_Y + 3, r, g, b);
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

static void draw_weather(const weather_snapshot_t *weather)
{
    if (!weather->valid) return;
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
    const uint8_t lit = (uint8_t)((seconds % SECONDS_H) + 1u);
    for (uint8_t row = 0; row < lit; ++row) {
        ws2812_matrix_set_pixel(SECONDS_X, SECONDS_Y + SECONDS_H - 1u - row,
                                DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
}

/* WS2812 发送失败时只重试一次，避免短暂总线错误留下半帧数据。 */
static esp_err_t show_frame_with_retry(void)
{
    esp_err_t err = ws2812_matrix_show(MATRIX_BRIGHTNESS);
    if (err == ESP_OK) return ESP_OK;

    APP_LOG_ERR(TAG, err, "WS2812 refresh failed, retrying");
    vTaskDelay(pdMS_TO_TICKS(1));

    err = ws2812_matrix_show(MATRIX_BRIGHTNESS);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "WS2812 refresh retry failed");
    }
    return err;
}

static void render_normal(void)
{
    time_snapshot_t time_snapshot = {0};
    weather_snapshot_t weather = {0};
    time_snapshot_get(&time_snapshot);
    weather_snapshot_get(&weather);

    time_t now = time(NULL);
    struct tm local_time;
    if (now != (time_t)-1) {
        localtime_r(&now, &local_time);
        time_snapshot.local_time = local_time;
        time_snapshot.unix_time = now;
    }

    ws2812_matrix_clear();
    draw_clock(&time_snapshot.local_time);
    draw_weather(&weather);
    draw_temperature(&weather);
    draw_seconds((uint8_t)time_snapshot.local_time.tm_sec);
    (void)show_frame_with_retry();
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

static void flow_animation(void)
{
    for (uint8_t phase = 0; phase < FLOW_FRAME_COUNT; ++phase) {
        ws2812_matrix_clear();
        for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
            for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
                uint8_t band = (uint8_t)((x + y + phase) & 0x0f);
                if (band < 4) {
                    uint8_t level = (uint8_t)(4 - band);
                    uint8_t intensity = (uint8_t)((level * DISPLAY_WHITE) / 4u);
                    ws2812_matrix_set_pixel(x, y, intensity, intensity, intensity);
                }
            }
        }
        (void)show_frame_with_retry();
        vTaskDelay(pdMS_TO_TICKS(FLOW_FRAME_DELAY_MS));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_events, DISPLAY_REFRESH, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(DISPLAY_REFRESH_PERIOD_MS));

        time_snapshot_t snapshot = {0};
        time_snapshot_get(&snapshot);
        time_t now = time(NULL);
        if (now != (time_t)-1) {
            localtime_r(&now, &snapshot.local_time);
            snapshot.unix_time = now;
        }
        if (flow_animation_due(&snapshot)) {
            flow_animation();
        }
        render_normal();
    }
}

/* 初始化 WS2812、事件组，并创建唯一拥有显示外设的 FreeRTOS 任务。 */
esp_err_t app_display_init(void)
{
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    esp_err_t err = ws2812_matrix_init();
    if (err != ESP_OK) return err;

    /* 先发送全黑帧，清除灯珠上电后的不确定状态。 */
    ws2812_matrix_clear();
    err = show_frame_with_retry();
    if (err != ESP_OK) return err;

    if (xTaskCreate(display_task, "display_task", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_events, DISPLAY_REFRESH);
    return ESP_OK;
}

/* 其他任务只发送刷新事件，不直接操作 WS2812。 */
void app_display_request_refresh(void)
{
    if (s_events) xEventGroupSetBits(s_events, DISPLAY_REFRESH);
}
