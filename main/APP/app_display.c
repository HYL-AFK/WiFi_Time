#include "app_display.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "common/time_snapshot.h"
#include "common/weather_snapshot.h"
#include "drivers/ws2812_matrix.h"

#define DISPLAY_REFRESH BIT0

#define CLOCK_X 0
#define CLOCK_Y 0
#define CLOCK_W 13
#define CLOCK_H 5

#define WEATHER_X 0
#define WEATHER_Y 10
#define WEATHER_W 6
#define WEATHER_H 6

#define DATA_X 7
#define DATA_Y 10
#define DATA_W 6
#define DATA_H 5

#define DEGREE_X 13
#define DEGREE_Y 11
#define DEGREE_W 2
#define DEGREE_H 2

#define SECONDS_X 15
#define SECONDS_Y 0
#define SECONDS_H 15

#define MATRIX_BRIGHTNESS 25
#define FLOW_FRAME_COUNT 32
#define FLOW_FRAME_DELAY_MS 25

static EventGroupHandle_t s_events;
static int64_t s_last_flow_minute = -1;

/* 3×5 字模正好填满 13×5 的 HH:MM 区域。 */
static const uint8_t s_digit_3x5[10][5] = {
    {0x07, 0x05, 0x05, 0x05, 0x07},
    {0x02, 0x06, 0x02, 0x02, 0x07},
    {0x07, 0x01, 0x07, 0x04, 0x07},
    {0x07, 0x01, 0x07, 0x01, 0x07},
    {0x05, 0x05, 0x07, 0x01, 0x01},
    {0x07, 0x04, 0x07, 0x01, 0x07},
    {0x07, 0x04, 0x07, 0x05, 0x07},
    {0x07, 0x01, 0x02, 0x02, 0x02},
    {0x07, 0x05, 0x07, 0x05, 0x07},
    {0x07, 0x05, 0x07, 0x01, 0x07},
};

/* 温度区域使用 3×3 紧凑数字，底部两行显示湿度比例条。 */
static const uint8_t s_digit_3x3[10][3] = {
    {0x07, 0x05, 0x07}, {0x02, 0x06, 0x02}, {0x07, 0x01, 0x07},
    {0x07, 0x03, 0x07}, {0x05, 0x07, 0x01}, {0x07, 0x06, 0x07},
    {0x06, 0x04, 0x07}, {0x07, 0x01, 0x01}, {0x07, 0x07, 0x07},
    {0x07, 0x03, 0x03},
};

static const uint8_t s_weather_clear[6] = {0x04, 0x11, 0x0e, 0x1c, 0x0e, 0x11};
static const uint8_t s_weather_cloud[6] = {0x00, 0x0c, 0x1e, 0x3f, 0x3f, 0x00};
static const uint8_t s_weather_rain[6] = {0x0c, 0x1e, 0x3f, 0x15, 0x2a, 0x15};
static const uint8_t s_weather_snow[6] = {0x08, 0x2a, 0x1c, 0x2a, 0x08, 0x00};

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

static void draw_digit_3x3(uint8_t x, uint8_t y, uint8_t digit,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if (digit > 9) return;
    for (uint8_t row = 0; row < 3; ++row) {
        for (uint8_t col = 0; col < 3; ++col) {
            if (s_digit_3x3[digit][row] & (1u << (2u - col))) {
                ws2812_matrix_set_pixel(x + col, y + row, r, g, b);
            }
        }
    }
}

static void draw_clock(const struct tm *local_time)
{
    const uint8_t hour = (uint8_t)local_time->tm_hour;
    const uint8_t minute = (uint8_t)local_time->tm_min;
    const uint8_t r = 0;
    const uint8_t g = 25;
    const uint8_t b = 90;

    draw_digit_3x5(CLOCK_X, CLOCK_Y, hour / 10, r, g, b);
    draw_digit_3x5(CLOCK_X + 3, CLOCK_Y, hour % 10, r, g, b);
    ws2812_matrix_set_pixel(6, 1, r, g, b);
    ws2812_matrix_set_pixel(6, 3, r, g, b);
    draw_digit_3x5(CLOCK_X + 7, CLOCK_Y, minute / 10, r, g, b);
    draw_digit_3x5(CLOCK_X + 10, CLOCK_Y, minute % 10, r, g, b);
}

static const uint8_t *weather_glyph(int weather_code)
{
    if (weather_code == 0) return s_weather_clear;
    if (weather_code >= 71 && weather_code <= 86) return s_weather_snow;
    if ((weather_code >= 51 && weather_code <= 67) ||
        (weather_code >= 80 && weather_code <= 99)) return s_weather_rain;
    return s_weather_cloud;
}

static void draw_weather(const weather_snapshot_t *weather)
{
    if (!weather->valid) return;
    const uint8_t *glyph = weather_glyph(weather->weather_code);
    for (uint8_t row = 0; row < WEATHER_H; ++row) {
        for (uint8_t col = 0; col < WEATHER_W; ++col) {
            if (glyph[row] & (1u << (5u - col))) {
                ws2812_matrix_set_pixel(WEATHER_X + col, WEATHER_Y + row, 55, 25, 5);
            }
        }
    }
}

static void draw_temperature_humidity(const weather_snapshot_t *weather)
{
    if (!weather->valid) return;

    int temperature = (int)(weather->temperature_c >= 0.0f ?
                            weather->temperature_c + 0.5f :
                            weather->temperature_c - 0.5f);
    if (temperature < 0) temperature = 0;
    if (temperature > 99) temperature = 99;
    draw_digit_3x3(DATA_X, DATA_Y, (uint8_t)(temperature / 10), 80, 18, 3);
    draw_digit_3x3(DATA_X + 3, DATA_Y, (uint8_t)(temperature % 10), 80, 18, 3);

    int humidity = weather->humidity;
    if (humidity < 0) humidity = 0;
    if (humidity > 100) humidity = 100;
    uint8_t width = (uint8_t)((humidity * DATA_W + 99) / 100);
    set_rect(DATA_X, DATA_Y + 3, width, 2, 20, 60, 20);

    /* 度号必须是实心 2×2，起点固定为 (13,11)。 */
    set_rect(DEGREE_X, DEGREE_Y, DEGREE_W, DEGREE_H, 80, 18, 3);
}

static void draw_seconds(uint8_t seconds)
{
    const uint8_t lit = (uint8_t)(1u + ((seconds % 60u) * SECONDS_H) / 60u);
    for (uint8_t row = 0; row < lit; ++row) {
        ws2812_matrix_set_pixel(SECONDS_X, SECONDS_Y + SECONDS_H - 1u - row,
                                70, 18, 10);
    }
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
    set_rect(0, 6, 13, 4, 3, 8, 18);
    draw_weather(&weather);
    draw_temperature_humidity(&weather);
    draw_seconds((uint8_t)time_snapshot.local_time.tm_sec);
    ws2812_matrix_show(MATRIX_BRIGHTNESS);
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
                    ws2812_matrix_set_pixel(x, y, (uint8_t)(level * 20),
                                            (uint8_t)(level * 8), (uint8_t)(level * 2));
                }
            }
        }
        ws2812_matrix_show(MATRIX_BRIGHTNESS);
        vTaskDelay(pdMS_TO_TICKS(FLOW_FRAME_DELAY_MS));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_events, DISPLAY_REFRESH, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(1000));

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
