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
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "common/app_log.h"
#include "common/runtime_event_log.h"
#include "common/time_snapshot.h"
#include "common/weather_snapshot.h"
#include "drivers/switch_input.h"
#include "drivers/ws2812_matrix.h"

#include "app/voice_faces.h"

#define DISPLAY_REFRESH BIT0
#define DISPLAY_NETWORK_SUCCESS BIT1
#define DISPLAY_NETWORK_FAILURE BIT2
#define DISPLAY_ALL_EVENTS (DISPLAY_REFRESH | DISPLAY_NETWORK_SUCCESS | DISPLAY_NETWORK_FAILURE)

#define CLOCK_X 0
#define CLOCK_Y 0
#define CLOCK_W 15
#define CLOCK_H 9
/* 24 小时制大字布局（4 位数字 + 冒号共 15 列，x15 给秒条）：
   时十位(0..2) 空(3) 时个位(4..6) 冒号(7) 分十位(8..10) 空(11) 分个位(12..14)。 */
#define CLOCK_HOUR_TENS_X 0
#define CLOCK_HOUR_ONES_X 4
#define CLOCK_COLON_X 7
#define CLOCK_MINUTE_TENS_X 8
#define CLOCK_MINUTE_ONES_X 12

#define WEATHER_X 0
#define WEATHER_Y 10 /* 天气图标占左下角 5x5。 */
#define WEATHER_W 5
#define WEATHER_H 5
#define WEATHER_PLACEHOLDER_Y (WEATHER_Y + 2)

#define TEMP_TENS_X 5
#define TEMP_ONES_X 8
#define TEMP_Y 10

/* 负温时数字右移 1 格给负号腾位。 */
#define NEGATIVE_X 5
#define NEGATIVE_Y 12

/* 度号只亮 1 个灯，紧贴温度个位右侧，与数字同一行。 */
#define DEGREE_X 11
#define DEGREE_Y 10
#define DEGREE_W 1
#define DEGREE_H 1

/* 闹钟标志：3×3，按领导手绘图：四角点亮 + 中间一横（双铃、表盘、双脚）。
   位置靠右、在底部区（y10~14）内垂直居中。 */
#define ALARM_ICON_X 13
#define ALARM_ICON_Y 11
#define ALARM_ICON_W 3
#define ALARM_ICON_H 3

/* 进度条改成底部横条：y15 的 x0~13 共 14 格，最右 (14,15)(15,15) 留给状态灯组。 */
#define SECONDS_X 0
#define SECONDS_Y 15
#define SECONDS_W 14
#define DISPLAY_STATUS_LAMP_X 15
#define DISPLAY_STATUS_LAMP_Y 15
/* 故障灯组从最右 (15,15) 起向左延伸：灯1=Wi-Fi 故障，灯2=天气接口故障。 */
#define DISPLAY_STATUS_LAMP_FAULT_SLOTS 2
#define DISPLAY_STATUS_LAMP_FAULT_LAST_X \
    (DISPLAY_STATUS_LAMP_X - (DISPLAY_STATUS_LAMP_FAULT_SLOTS - 1))

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
/* 晴天图标三档颜色：光芒暗琥珀 / 本体橙 / 高光浅黄，比例取自设计稿 #9A5808 / #FFA51F / #FFF6C8。 */
#define SUN_RAY_R 62
#define SUN_RAY_G 34
#define SUN_RAY_B 4
#define SUN_BODY_R 100
#define SUN_BODY_G 64
#define SUN_BODY_B 10
#define SUN_CORE_R 100
#define SUN_CORE_G 94
#define SUN_CORE_B 62
#define SUN_CORNER_R 74
#define SUN_CORNER_G 42
#define SUN_CORNER_B 5
/* 闪电：亮黄 #FFD24A 按面板亮度缩放。 */
#define THUNDER_R 100
#define THUNDER_G 82
#define THUNDER_B 29
#define DISPLAY_NORMAL_R DISPLAY_GREEN_R
#define DISPLAY_NORMAL_G DISPLAY_GREEN_G
#define DISPLAY_NORMAL_B DISPLAY_GREEN_B
#define DISPLAY_REFRESH_PERIOD_MS 50
#define STARTUP_FRAME_PERIOD_MS 33
/* 尾迹加长后更平缓，既能掩盖离散跳格，也让两环的相对位移更不明显。 */
#define STARTUP_RING_TRAIL_LENGTH 9
#define STARTUP_RING_DIM 16
/* 内框比外框暗一档，做出双层边框的层次感。 */
#define STARTUP_INNER_DIM 9
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
static bool s_force_render_once;
static const char *TAG = "app_display";

typedef enum {
    DISPLAY_MODE_BOOT = 0,
    DISPLAY_MODE_MAIN,
} display_mode_t;

/* 主界面页面：拨动开关单击在三页之间轮换。
   注：语音模块接入后，语音页应改为由语音事件驱动，不再进开关循环。 */
typedef enum {
    DISPLAY_PAGE_MAIN = 0,
    DISPLAY_PAGE_ALARM,
    DISPLAY_PAGE_VOICE,
    DISPLAY_PAGE_COUNT,
} display_page_t;

/* ===== 闹钟页（左右结构，去掉 ON/OFF 字母）=====
   时间直接复用主页面那套 CLOCK_* 坐标与 3×9 字模（y0~8），两页显示逻辑一致；
   左下 7×7 指针图标（y9~15），中间是 7 档音量柱，最右 15 格秒条。
   开 = 亮红且指针在走；关 = 中性灰且不画指针（停走）。 */
#define ALARM_PAGE_SECONDS_X 15
#define ALARM_PAGE_SECONDS_H 15
#define ALARM_PAGE_ICON_X 0
#define ALARM_PAGE_ICON_Y 9
#define ALARM_PAGE_ICON_W 7
#define ALARM_PAGE_ICON_H 7

/* 音量指示列宽不再固定为区块，改由 3 条竖线的坐标表控制（见 draw_volume_bars）。 */
#define ALARM_PAGE_VOLUME_MAX 7

#define ALARM_PAGE_ON_R 255
#define ALARM_PAGE_ON_G 96
#define ALARM_PAGE_ON_B 84
#define ALARM_PAGE_OFF_R 150
#define ALARM_PAGE_OFF_G 160
#define ALARM_PAGE_OFF_B 176
#define ALARM_PAGE_SECONDS_R 40
#define ALARM_PAGE_SECONDS_G 58
#define ALARM_PAGE_SECONDS_B 84

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

/* 开机动画路径：点阵外圈的正方形边框（x/y ∈ 1..14，顺时针一圈 52 点）。 */
static const display_point_t s_startup_clock_ring[] = {
    /* 上边：x = 1..14, y = 1 */
    {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
    {9, 1}, {10, 1}, {11, 1}, {12, 1}, {13, 1}, {14, 1},
    /* 右边：x = 14, y = 2..14 */
    {14, 2}, {14, 3}, {14, 4}, {14, 5}, {14, 6}, {14, 7}, {14, 8},
    {14, 9}, {14, 10}, {14, 11}, {14, 12}, {14, 13}, {14, 14},
    /* 下边：y = 14, x = 13..1 */
    {13, 14}, {12, 14}, {11, 14}, {10, 14}, {9, 14}, {8, 14}, {7, 14},
    {6, 14}, {5, 14}, {4, 14}, {3, 14}, {2, 14}, {1, 14},
    /* 左边：x = 1, y = 13..2 */
    {1, 13}, {1, 12}, {1, 11}, {1, 10}, {1, 9}, {1, 8}, {1, 7},
    {1, 6}, {1, 5}, {1, 4}, {1, 3}, {1, 2},
};

/* 内框：x/y ∈ 2..13 的正方形，紧贴外框（中间不留空隙），形成 2 像素厚的双层边框。 */
static const display_point_t s_startup_clock_inner[] = {
    /* 上边：y = 2, x = 2..13 */
    {2, 2}, {3, 2}, {4, 2}, {5, 2}, {6, 2}, {7, 2}, {8, 2}, {9, 2},
    {10, 2}, {11, 2}, {12, 2}, {13, 2},
    /* 右边：x = 13, y = 3..13 */
    {13, 3}, {13, 4}, {13, 5}, {13, 6}, {13, 7}, {13, 8}, {13, 9},
    {13, 10}, {13, 11}, {13, 12}, {13, 13},
    /* 下边：y = 13, x = 12..2 */
    {12, 13}, {11, 13}, {10, 13}, {9, 13}, {8, 13}, {7, 13}, {6, 13},
    {5, 13}, {4, 13}, {3, 13}, {2, 13},
    /* 左边：x = 2, y = 12..3 */
    {2, 12}, {2, 11}, {2, 10}, {2, 9}, {2, 8}, {2, 7}, {2, 6},
    {2, 5}, {2, 4}, {2, 3},
};

static const uint8_t s_startup_trail_levels[STARTUP_RING_TRAIL_LENGTH] = {
    DISPLAY_WHITE, 72, 55, 42, 32, 24, 18, 13, 10,
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

/* 时间大字体：3×9。除 1、9 外的字形保持原直角结构，仅 1、9 做了带弧度的处理。 */
static const uint8_t s_digit_3x9[10][9] = {
    {0x07, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x07},
    {0x02, 0x06, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x07},
    {0x07, 0x01, 0x01, 0x01, 0x07, 0x04, 0x04, 0x04, 0x07},
    {0x07, 0x01, 0x01, 0x01, 0x07, 0x01, 0x01, 0x01, 0x07},
    {0x05, 0x05, 0x05, 0x05, 0x07, 0x01, 0x01, 0x01, 0x01},
    {0x07, 0x04, 0x04, 0x04, 0x07, 0x01, 0x01, 0x01, 0x07},
    {0x07, 0x04, 0x04, 0x04, 0x07, 0x05, 0x05, 0x05, 0x07},
    {0x07, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
    {0x07, 0x05, 0x05, 0x05, 0x07, 0x05, 0x05, 0x05, 0x07},
    {0x07, 0x05, 0x05, 0x05, 0x07, 0x01, 0x01, 0x01, 0x06},
};

/* 每个天气图标都是 5x5 位图，最高位对应图标最左列。 */
/* 闹钟标志（3×3，按指定点阵）：
     · █ █     第 1 列只亮第 2 个；第 2 列亮第 1、3 个；第 3 列全亮。
     █ · █
     · █ █ */
static const uint8_t s_alarm_icon[ALARM_ICON_H] = {
    0b011, 0b101, 0b011};
static const uint8_t s_weather_clear[WEATHER_H] = {0x04, 0x11, 0x0e, 0x11, 0x04}; /* 晴天 */
static const uint8_t s_weather_cloud[WEATHER_H] = {0x00, 0x0e, 0x1f, 0x1f, 0x00}; /* 多云/雾 */
static const uint8_t s_weather_rain[WEATHER_H] = {0x04, 0x0e, 0x1f, 0x0a, 0x15}; /* 降雨 */
static const uint8_t s_weather_snow[WEATHER_H] = {0x04, 0x15, 0x0e, 0x15, 0x04}; /* 降雪 */
static const uint8_t s_weather_thunder[WEATHER_H] = {0x04, 0x08, 0x1e, 0x04, 0x08}; /* 雷：单列折线闪电，不带云 */

/* 晴天用分层图上色：0=不亮，1=光芒（暗琥珀），2=本体（橙），3=高光（浅黄），4=本体圆角（暗橙）。
   3×3 本体（四角磨圆）+ 四条边各 3 格光芒，四角留空，铺满 5×5。 */
static const uint8_t s_weather_clear_levels[WEATHER_H][WEATHER_W] = {
    {0, 1, 1, 1, 0},
    {1, 4, 2, 4, 1},
    {1, 2, 3, 2, 1},
    {1, 4, 2, 4, 1},
    {0, 1, 1, 1, 0},
};

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

static void draw_digit_3x9(uint8_t x, uint8_t y, uint8_t digit,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if (digit > 9) return;
    for (uint8_t row = 0; row < CLOCK_H; ++row) {
        for (uint8_t col = 0; col < 3; ++col) {
            if (s_digit_3x9[digit][row] & (1u << (2u - col))) {
                ws2812_matrix_set_pixel(x + col, y + row, r, g, b);
            }
        }
    }
}

/* 时间颜色已定稿：豆沙橙。TIME_COLOR_ROTATE 置 1 可恢复调色轮播。 */
#define TIME_COLOR_ROTATE 0
#define TIME_COLOR_HOLD_MS 4000

typedef struct {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} display_time_color_t;

/* 候选颜色。名称前的数字是首轮编号，便于回溯调色过程。 */
static const display_time_color_t s_time_palette[] = {
    {"豆沙橙", 214, 120, 90},
};
#define TIME_PALETTE_COUNT (sizeof(s_time_palette) / sizeof(s_time_palette[0]))

static uint8_t current_time_color_index(void)
{
#if TIME_COLOR_ROTATE
    const uint32_t slot =
        (uint32_t)(xTaskGetTickCount() / pdMS_TO_TICKS(TIME_COLOR_HOLD_MS));
    return (uint8_t)(slot % TIME_PALETTE_COUNT);
#else
    return 0; /* 豆沙橙 */
#endif
}

/* 24 小时制：HH:MM，00..23，不做 12 小时换算也不带 AM/PM 标记。 */
static void draw_clock(const struct tm *local_time)
{
    const uint8_t hour = (uint8_t)local_time->tm_hour;
    const uint8_t minute = (uint8_t)local_time->tm_min;
    const display_time_color_t *tc = &s_time_palette[current_time_color_index()];
    const uint8_t r = tc->r;
    const uint8_t g = tc->g;
    const uint8_t b = tc->b;

    draw_digit_3x9(CLOCK_HOUR_TENS_X, CLOCK_Y, (uint8_t)(hour / 10), r, g, b);
    draw_digit_3x9(CLOCK_HOUR_ONES_X, CLOCK_Y, (uint8_t)(hour % 10), r, g, b);
    if ((local_time->tm_sec & 1) == 0) {
        ws2812_matrix_set_pixel(CLOCK_COLON_X, CLOCK_Y + 3, r, g, b);
        ws2812_matrix_set_pixel(CLOCK_COLON_X, CLOCK_Y + 6, r, g, b);
    }
    draw_digit_3x9(CLOCK_MINUTE_TENS_X, CLOCK_Y, (uint8_t)(minute / 10), r, g, b);
    draw_digit_3x9(CLOCK_MINUTE_ONES_X, CLOCK_Y, (uint8_t)(minute % 10), r, g, b);
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
    if (weather->weather_code == 0) {
        /* 晴天：按层级上色，光芒/本体/高光三档不同颜色。 */
        for (uint8_t row = 0; row < WEATHER_H; ++row) {
            for (uint8_t col = 0; col < WEATHER_W; ++col) {
                uint8_t r = 0, g = 0, b = 0;
                switch (s_weather_clear_levels[row][col]) {
                case 1: r = SUN_RAY_R; g = SUN_RAY_G; b = SUN_RAY_B; break;
                case 2: r = SUN_BODY_R; g = SUN_BODY_G; b = SUN_BODY_B; break;
                case 3: r = SUN_CORE_R; g = SUN_CORE_G; b = SUN_CORE_B; break;
                case 4: r = SUN_CORNER_R; g = SUN_CORNER_G; b = SUN_CORNER_B; break;
                default: continue;
                }
                ws2812_matrix_set_pixel(WEATHER_X + col, WEATHER_Y + row, r, g, b);
            }
        }
        return;
    }
    const uint8_t *glyph = weather_glyph(weather->weather_code);
    /* 闪电用亮黄，其余图标暂时仍是白色。 */
    uint8_t r = DISPLAY_WHITE;
    uint8_t g = DISPLAY_WHITE;
    uint8_t b = DISPLAY_WHITE;
    if (weather->weather_code >= 95 && weather->weather_code <= 99) {
        r = THUNDER_R;
        g = THUNDER_G;
        b = THUNDER_B;
    }
    for (uint8_t row = 0; row < WEATHER_H; ++row) {
        for (uint8_t col = 0; col < WEATHER_W; ++col) {
            if (glyph[row] & (1u << (WEATHER_W - 1u - col))) {
                ws2812_matrix_set_pixel(WEATHER_X + col, WEATHER_Y + row, r, g, b);
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

    /* 负温时数字整体右移 1 格，给负号腾位。 */
    const uint8_t shift = negative ? 1u : 0u;
    if (negative) {
        ws2812_matrix_set_pixel(NEGATIVE_X, NEGATIVE_Y,
                                DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
    /* 个位数温度不补 0：十位留空。 */
    if (temperature >= 10) {
        draw_digit_3x5((uint8_t)(TEMP_TENS_X + shift), TEMP_Y, (uint8_t)(temperature / 10),
                       DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
    draw_digit_3x5((uint8_t)(TEMP_ONES_X + shift), TEMP_Y, (uint8_t)(temperature % 10),
                   DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);

    /* 度号属于温度显示，即使尚未联网使用 RTC 温度时也必须显示。 */
    set_rect((uint8_t)(DEGREE_X + shift), DEGREE_Y, DEGREE_W, DEGREE_H,
             DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
}
static void draw_alarm_icon(void)
{
    /* 闹钟标志用粉色，与白色温度区分开。 */
    for (uint8_t row = 0; row < ALARM_ICON_H; ++row) {
        for (uint8_t col = 0; col < ALARM_ICON_W; ++col) {
            if (s_alarm_icon[row] & (1u << (ALARM_ICON_W - 1u - col))) {
                ws2812_matrix_set_pixel(ALARM_ICON_X + col, ALARM_ICON_Y + row,
                                        DISPLAY_PINK_R, DISPLAY_PINK_G, DISPLAY_PINK_B);
            }
        }
    }
}

static void draw_seconds(uint8_t seconds)
{
#if TIME_COLOR_ROTATE
    /* 调色期间的辅助指示：进度条从左往右点亮 index+1 格 = 当前是第几个颜色。 */
    (void)seconds;
    const uint8_t lit = (uint8_t)(current_time_color_index() + 1u);
    for (uint8_t col = 0; col < lit && col < SECONDS_W; ++col) {
        ws2812_matrix_set_pixel((uint8_t)(SECONDS_X + col), SECONDS_Y,
                                DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
#else
    /* 底部横条：从左往右填充，填满 14 格后反向回辋。 */
    const uint8_t cycle = (uint8_t)(seconds / SECONDS_W);
    const uint8_t step = (uint8_t)(seconds % SECONDS_W);
    const uint8_t lit = (cycle & 1u) == 0u
                            ? (uint8_t)(step + 1u)
                            : (uint8_t)(SECONDS_W - step - 1u);
    for (uint8_t col = 0; col < lit; ++col) {
        const uint8_t x = (cycle & 1u) == 0u
                              ? (uint8_t)(SECONDS_X + col)
                              : (uint8_t)(SECONDS_X + SECONDS_W - 1u - col);
        ws2812_matrix_set_pixel(x, SECONDS_Y, DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE);
    }
#endif
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
        /* 故障灯组：最右 (15,15) 起向左点亮红灯，每类故障一盏，便于区分故障来源。
           Wi-Fi 故障 = 1 盏；Wi-Fi 正常但天气接口故障 = 2 盏。 */
        const uint8_t lamp_count = wifi_fault ? 1U : 2U;
        for (uint8_t i = 0; i < lamp_count; ++i) {
            ws2812_matrix_set_pixel((uint8_t)(DISPLAY_STATUS_LAMP_X - i),
                                    DISPLAY_STATUS_LAMP_Y,
                                    DISPLAY_RED_R, DISPLAY_RED_G, DISPLAY_RED_B);
        }
        return;
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

/* 内外两环周长不同（52 / 44），不能按“整圈相位”取格点：
   外框上边占 14/52 = 26.9%，内框上边占 12/44 = 27.3%，拐角会错开。
   改成按“第几条边 + 边内比例”插值（arc-length 分段参数化），
   拐角同时到达、边中点也同时到达。下面分别是每条边的首格偏移与格数（首格含拐角）。 */
typedef struct {
    uint8_t offset;
    uint8_t length;
} startup_side_t;

static const startup_side_t s_ring_sides[4] = {{0, 14}, {14, 13}, {27, 13}, {40, 12}};
static const startup_side_t s_inner_sides[4] = {{0, 12}, {12, 11}, {23, 11}, {34, 10}};

/* 两环共用的“每条边步数”，取两环中格数较多的一边减一（上/右/下/左）。
   这样两环在每一帧同时向前跳一格，不会出现一边先跳、另一边后跟的“追赶”观感。 */
static const uint8_t s_side_steps[4] = {13, 12, 12, 11};

/* 沿一圆环绘制亮点 + 尾迹。
   亮度全部恒定（头格满亮、尾迹按固定阶梯），不做亮度插值：
   亚格点亮度插值会让头格亮度每帧在满亮与半亮之间摆动，小灯珠上看起来就是频闪。*/
static void draw_startup_ring(const display_point_t *ring, size_t count,
                              const startup_side_t *sides, uint32_t phase_q16,
                              uint8_t base_dim, uint8_t scale_percent)
{
    for (size_t i = 0; i < count; ++i) {
        const display_point_t p = ring[i];
        ws2812_matrix_set_pixel(p.x, p.y, base_dim, base_dim, base_dim);
    }

    const uint32_t side = (phase_q16 >> 14) & 3u;
    const uint32_t frac = phase_q16 & 0x3FFFu;
    const startup_side_t *s = &sides[side];
    const uint32_t steps = s_side_steps[side];
    const uint32_t last = (uint32_t)s->length - 1u;
    /* 边内步号（0..steps-1）；头格按四舍五入落格，两环在统一步数下保持同列/同行。 */
    const uint32_t step = (frac * steps) / 16384u;
    const size_t head = (size_t)(s->offset + (step * last + steps / 2u) / steps);

    for (size_t k = 0; k < STARTUP_RING_TRAIL_LENGTH; ++k) {
        const uint8_t level =
            scale_startup_level(s_startup_trail_levels[k], scale_percent);
        const size_t index = (head + count - k) % count;
        const display_point_t p = ring[index];
        ws2812_matrix_set_pixel(p.x, p.y, level, level, level);
    }
}

static void render_startup_frame_scaled(uint32_t elapsed_ms, uint8_t scale_percent)
{
    s_last_output_main = false;
    ws2812_matrix_clear();

    const size_t ring_count =
        sizeof(s_startup_clock_ring) / sizeof(s_startup_clock_ring[0]);
    const size_t inner_count =
        sizeof(s_startup_clock_inner) / sizeof(s_startup_clock_inner[0]);

    const uint32_t ring_dim = scale_startup_level(STARTUP_RING_DIM, scale_percent);
    const uint32_t inner_dim = scale_startup_level(STARTUP_INNER_DIM, scale_percent);

    /* 用同一个圆内相位驱动两环；每环各自按“边内比例”换算连续位置，
       再过亚格点插值到格子，避免整格跳动带来的“一方追赶另一方”。 */
    const uint32_t lap_ms = elapsed_ms % STARTUP_RING_LAP_DURATION_MS;
    const uint32_t phase_q16 =
        (uint32_t)(((uint64_t)lap_ms << 16) / STARTUP_RING_LAP_DURATION_MS);

    draw_startup_ring(s_startup_clock_ring, ring_count, s_ring_sides, phase_q16,
                      (uint8_t)ring_dim, scale_percent);
    draw_startup_ring(s_startup_clock_inner, inner_count, s_inner_sides, phase_q16,
                      (uint8_t)inner_dim, scale_percent);

    (void)show_frame_with_retry();
}

static void render_startup_frame(uint32_t elapsed_ms)
{
    render_startup_frame_scaled(elapsed_ms, 100);
}

static void render_startup_fade_out(uint32_t elapsed_ms)
{
    render_startup_frame_scaled(elapsed_ms, 67);
    vTaskDelay(pdMS_TO_TICKS(STARTUP_FADE_STEP_MS));
    render_startup_frame_scaled(elapsed_ms, 33);
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

    static uint8_t s_last_color_index = 0xFFu;
    const uint8_t color_index = current_time_color_index();
    if (color_index != s_last_color_index) {
        s_last_color_index = color_index;
        APP_LOGI(TAG, "time color %u/%u %s rgb=(%u,%u,%u)", (unsigned)(color_index + 1u),
                 (unsigned)TIME_PALETTE_COUNT, s_time_palette[color_index].name,
                 s_time_palette[color_index].r, s_time_palette[color_index].g,
                 s_time_palette[color_index].b);
    }

    ws2812_matrix_clear();
    draw_clock(&time_snapshot.local_time);
    draw_weather(&weather);
    draw_temperature(&weather);
    draw_alarm_icon();
    draw_seconds((uint8_t)time_snapshot.local_time.tm_sec);
    draw_status_lamp();

    ws2812_frame_diagnostics_t diag = {0};
    ws2812_matrix_get_frame_diagnostics(&diag);
    const bool status_lamp_pixel =
        diag.y == DISPLAY_STATUS_LAMP_Y &&
        diag.x >= DISPLAY_STATUS_LAMP_FAULT_LAST_X &&
        diag.x <= DISPLAY_STATUS_LAMP_X;
    /* 天气图标带颜色（如晴天图标），属于预期彩色区域；区域外的彩色仍当作传输损坏丢弃。 */
    const bool weather_icon_pixel =
        diag.y >= WEATHER_Y && diag.y < WEATHER_Y + WEATHER_H &&
        diag.x < WEATHER_X + WEATHER_W;
    /* 时间区在调色期间也是彩色，同样放行。 */
    const bool clock_pixel = diag.y < CLOCK_Y + CLOCK_H && diag.x < CLOCK_X + CLOCK_W;
    /* 闹钟标志是粉色，属于预期彩色区域。 */
    const bool alarm_icon_pixel =
        diag.y >= ALARM_ICON_Y && diag.y < ALARM_ICON_Y + ALARM_ICON_H &&
        diag.x >= ALARM_ICON_X && diag.x < ALARM_ICON_X + ALARM_ICON_W;
    /* 健康任务请求恢复时强制显示一次，保证画面不会被诊断保护永久冻结。 */
    const bool force_once = s_force_render_once;
    s_force_render_once = false;
    if (!diag.grayscale && !status_lamp_pixel && !weather_icon_pixel &&
        !clock_pixel && !alarm_icon_pixel && !force_once) {
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
            /* 续帧也双发：单次传输偶发位错误会被下一发立刻覆盖。 */
            esp_err_t keepalive_err = show_frame_force_with_retry();
            if (keepalive_err == ESP_OK) {
                esp_rom_delay_us(WS2812_DUPLICATE_LATCH_US);
                keepalive_err = show_frame_force_with_retry();
            }
            if (keepalive_err == ESP_OK) {
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
    s_animation.current_frame = 0;
    s_last_output_main = false;
    /* 动画结束后必须交回一张正常主画面；未成功传输时记录事件供现场诊断。 */
    (void)render_normal(xTaskGetTickCount(), NULL);
    if (!s_last_output_main) {
        display_record_event(RUNTIME_EVENT_DISPLAY_ERROR,
                             (int32_t)s_animation.attempted_frames,
                             (int32_t)s_animation.max_refresh_ms,
                             DISPLAY_ANIMATION_HANDOFF_FAILED);
    }
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

/* ===== 语音页 =====
   整屏一张 16×16 表情，按语音状态切换；thinking 不走表情，用加载转圈动画。
   语音模组尚未接入，状态暂由 HTTP /voice?s=N 或开关先手动指定。 */
typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_AWAKE,
    VOICE_STATE_LISTEN,
    VOICE_STATE_THINKING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_MUTED,
    VOICE_STATE_UNKNOWN,
    VOICE_STATE_COUNT,
} voice_state_t;

/* 状态 → 表情索引。thinking 用加载动画，这里的值是占位，渲染时走 spinner 分支。
   11 待机沿用原静音那张；23/24 是为静音、聆听新增的两张。 */
static const uint8_t s_voice_state_face[VOICE_STATE_COUNT] = {
    [VOICE_STATE_IDLE] = 11,
    [VOICE_STATE_AWAKE] = 2,
    [VOICE_STATE_LISTEN] = 24,
    [VOICE_STATE_THINKING] = 0,
    [VOICE_STATE_SPEAKING] = 14,
    [VOICE_STATE_MUTED] = 23,
    [VOICE_STATE_UNKNOWN] = 16,
};
static const char *const s_voice_state_name[VOICE_STATE_COUNT] = {
    "idle", "awake", "listen", "thinking", "speaking", "muted", "unknown"};

/* 处理中的加载动画：8 个点围成半径约 6 的圆，每帧 1 个满亮 + 2 档尾迹，转一圈 8 帧。 */
#define VOICE_SPINNER_STEPS 8
#define VOICE_SPINNER_FRAME_MS 90

static const uint8_t s_spinner_xy[VOICE_SPINNER_STEPS][2] = {
    {8, 1}, {12, 4}, {14, 8}, {12, 11}, {8, 14}, {4, 11}, {1, 8}, {4, 4}};

static voice_state_t s_voice_state = VOICE_STATE_IDLE;
static volatile int s_voice_state_request = -1;
static uint8_t s_spinner_frame;
static int64_t s_spinner_last_us;

void app_display_set_voice_state(int state)
{
    if (state < 0 || state >= (int)VOICE_STATE_COUNT) return;
    s_voice_state_request = state;
    app_display_request_refresh();
}

/* 转圈：不足一帧间隔就不重发，画面保持上一帧。 */
static void render_voice_spinner(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_spinner_last_us != 0 &&
        now - s_spinner_last_us < (int64_t)VOICE_SPINNER_FRAME_MS * 1000) {
        return;
    }
    s_spinner_last_us = now;

    static const uint8_t k_level[3] = {100, 48, 20};
    const display_time_color_t *tc = &s_time_palette[0];

    ws2812_matrix_clear();
    for (uint8_t i = 0; i < 3; ++i) {
        const uint8_t idx =
            (uint8_t)((s_spinner_frame + VOICE_SPINNER_STEPS - i) % VOICE_SPINNER_STEPS);
        const uint8_t r = (uint8_t)((tc->r * k_level[i]) / 100U);
        const uint8_t g = (uint8_t)((tc->g * k_level[i]) / 100U);
        const uint8_t b = (uint8_t)((tc->b * k_level[i]) / 100U);
        ws2812_matrix_set_pixel(s_spinner_xy[idx][0], s_spinner_xy[idx][1], r, g, b);
    }
    s_spinner_frame = (uint8_t)((s_spinner_frame + 1u) % VOICE_SPINNER_STEPS);

    if (show_frame_with_retry() != ESP_OK) {
        APP_LOGE(TAG, "voice spinner frame send failed");
    }
}

static void render_voice_page(void)
{
    if (s_voice_state == VOICE_STATE_THINKING) {
        render_voice_spinner();
        return;
    }

    const uint8_t face = s_voice_state_face[s_voice_state];
    const display_time_color_t *tc = &s_time_palette[0];

    ws2812_matrix_clear();
    for (uint8_t row = 0; row < MATRIX_HEIGHT; ++row) {
        const uint16_t bits = s_voice_faces[face][row];
        for (uint8_t col = 0; col < MATRIX_WIDTH; ++col) {
            if (bits & (1u << (MATRIX_WIDTH - 1u - col))) {
                ws2812_matrix_set_pixel(col, row, tc->r, tc->g, tc->b);
            }
        }
    }
    if (show_frame_with_retry() != ESP_OK) {
        APP_LOGE(TAG, "voice page frame send failed");
    }
}

/* ===== 闹钟页 ===== */

/* 闹钟设定值：目前由显示层持有，等闹钟服务（NVS 持久化 + 到点事件）接入后改成读服务。 */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t volume; /* 1~7 档 */
    bool enabled;
} alarm_page_state_t;

static alarm_page_state_t s_alarm_page = {.hour = 7, .minute = 30, .volume = 4, .enabled = true};

/* 来自调试接口（HTTP）的请求，由 display_task 在下一轮循环应用。
   -1 表示无请求；用 volatile 而非队列，是因为只传递最终状态，重复覆盖无副作用。 */
static volatile int s_page_request = -1;
static volatile int s_alarm_enabled_request = -1;
static volatile int s_alarm_volume_request = -1;

void app_display_set_page(int page)
{
    if (page < 0 || page >= (int)DISPLAY_PAGE_COUNT) return;
    s_page_request = page;
    app_display_request_refresh();
}

void app_display_set_alarm_enabled(bool on)
{
    s_alarm_enabled_request = on ? 1 : 0;
    app_display_request_refresh();
}

void app_display_set_alarm_volume(uint8_t volume)
{
    if (volume == 0 || volume > ALARM_PAGE_VOLUME_MAX) return;
    s_alarm_volume_request = volume;
    app_display_request_refresh();
}

/* 7×7 指针图标，bit6 = 最左列。开版带指针（中心+向右一格），关版指针停走。 */
static const uint16_t s_alarm_art_run[ALARM_PAGE_ICON_H] = {
    0x63, 0x3E, 0x41, 0x4D, 0x41, 0x3E, 0x22};
static const uint16_t s_alarm_art_stop[ALARM_PAGE_ICON_H] = {
    0x63, 0x3E, 0x41, 0x41, 0x41, 0x3E, 0x22};

static void draw_bitmap_rows(uint8_t x, uint8_t y, const uint16_t *rows, uint8_t width,
                             uint8_t height, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint8_t row = 0; row < height; ++row) {
        for (uint8_t col = 0; col < width; ++col) {
            if (rows[row] & (1u << (width - 1u - col))) {
                ws2812_matrix_set_pixel((uint8_t)(x + col), (uint8_t)(y + row), r, g, b);
            }
        }
    }
}

/* 音量指示：3 条由短到长的竖线，点亮条数随音量档位递增。
   比实心柱更轻，不会在点阵上形成色块。 */
static void draw_volume_bars(uint8_t volume)
{
    /* 3 条线的列号与高度，越靠右越高。 */
    static const uint8_t k_bar_x[3] = {9, 11, 13};
    static const uint8_t k_bar_h[3] = {3, 5, 7};
    /* 7 档映射到 3 条：1~2 一条，3~4 两条，5~7 三条。 */
    const uint8_t bars = (uint8_t)(volume <= 2u ? 1u : (volume <= 4u ? 2u : 3u));
    const display_time_color_t *tc = &s_time_palette[0];
    const uint8_t bottom = (uint8_t)(ALARM_PAGE_ICON_Y + ALARM_PAGE_ICON_H - 1u);

    for (uint8_t i = 0; i < 3; ++i) {
        /* 未点亮的那几条给一层很暗的底色：形状始终可辨，低音量时也不会只剩孤零零一根。 */
        const bool lit = i < bars;
        const uint8_t r = lit ? tc->r : (uint8_t)(tc->r / 6u);
        const uint8_t g = lit ? tc->g : (uint8_t)(tc->g / 6u);
        const uint8_t b = lit ? tc->b : (uint8_t)(tc->b / 6u);
        for (uint8_t row = 0; row < k_bar_h[i]; ++row) {
            ws2812_matrix_set_pixel(k_bar_x[i], (uint8_t)(bottom - row), r, g, b);
        }
    }
}

static void render_alarm_page(void)
{
    time_snapshot_t snapshot = {0};
    time_snapshot_get(&snapshot);

    /* 秒必须用本地时钟重新取：快照大约每分钟才更新一次，
       直接读 snapshot 的 tm_sec 会让秒条卡死不动（与主页面渲染保持一致）。 */
    struct tm now_tm = snapshot.local_time;
    if (time_snapshot_is_ready()) {
        const time_t now = time(NULL);
        struct tm local_time;
        if (now != (time_t)-1 && localtime_r(&now, &local_time) != NULL) {
            now_tm = local_time;
        }
    }

    const display_time_color_t *tc = &s_time_palette[0];
    const uint8_t r = tc->r, g = tc->g, b = tc->b;
    const uint8_t hour = (uint8_t)(s_alarm_page.hour % 24u);
    const uint8_t minute = (uint8_t)(s_alarm_page.minute % 60u);

    ws2812_matrix_clear();

    /* 设定时间：与主页面同坐标、同字模，始终两位小时。 */
    draw_digit_3x9(CLOCK_HOUR_TENS_X, CLOCK_Y, (uint8_t)(hour / 10u), r, g, b);
    draw_digit_3x9(CLOCK_HOUR_ONES_X, CLOCK_Y, (uint8_t)(hour % 10u), r, g, b);
    ws2812_matrix_set_pixel(CLOCK_COLON_X, (uint8_t)(CLOCK_Y + 3u), r, g, b);
    ws2812_matrix_set_pixel(CLOCK_COLON_X, (uint8_t)(CLOCK_Y + 6u), r, g, b);
    draw_digit_3x9(CLOCK_MINUTE_TENS_X, CLOCK_Y, (uint8_t)(minute / 10u), r, g, b);
    draw_digit_3x9(CLOCK_MINUTE_ONES_X, CLOCK_Y, (uint8_t)(minute % 10u), r, g, b);

    /* 最右 15 格秒条：按当前秒数逐格点亮，从下往上长。 */
    const uint8_t lit = (uint8_t)((now_tm.tm_sec % ALARM_PAGE_SECONDS_H) + 1);
    for (uint8_t i = 0; i < lit; ++i) {
        const uint8_t y = (uint8_t)(ALARM_PAGE_SECONDS_H - 1u - i);
        ws2812_matrix_set_pixel(ALARM_PAGE_SECONDS_X, y, ALARM_PAGE_SECONDS_R,
                                ALARM_PAGE_SECONDS_G, ALARM_PAGE_SECONDS_B);
    }

    /* 中间音量柱：填充图标与秒条之间的空白，对应铃声音量档位。 */
    draw_volume_bars(s_alarm_page.volume);

    /* 左下图标：开亮红带指针，关中性灰停走。 */
    const uint8_t icon_r = s_alarm_page.enabled ? ALARM_PAGE_ON_R : ALARM_PAGE_OFF_R;
    const uint8_t icon_g = s_alarm_page.enabled ? ALARM_PAGE_ON_G : ALARM_PAGE_OFF_G;
    const uint8_t icon_b = s_alarm_page.enabled ? ALARM_PAGE_ON_B : ALARM_PAGE_OFF_B;
    draw_bitmap_rows(ALARM_PAGE_ICON_X, ALARM_PAGE_ICON_Y,
                     s_alarm_page.enabled ? s_alarm_art_run : s_alarm_art_stop,
                     ALARM_PAGE_ICON_W, ALARM_PAGE_ICON_H, icon_r, icon_g, icon_b);

    if (show_frame_with_retry() != ESP_OK) {
        APP_LOGE(TAG, "alarm page frame send failed");
    }
}

static void display_task(void *arg)
{
    (void)arg;
    const TickType_t boot_started = xTaskGetTickCount();
    const TickType_t frame_period = pdMS_TO_TICKS(STARTUP_FRAME_PERIOD_MS);
    TickType_t next_frame = boot_started;
    TickType_t cycle_started = boot_started;
    uint32_t startup_elapsed_ms = 0;
    uint32_t boot_animation_cycle = 0;
    display_mode_t mode = DISPLAY_MODE_BOOT;
    display_page_t s_page = DISPLAY_PAGE_MAIN;
    network_notice_t pending_notice = NETWORK_NOTICE_NONE;

    for (;;) {
        const TickType_t wait_ticks = s_animation.active
                                          ? pdMS_TO_TICKS(FLOW_FRAME_DELAY_MS)
                                          : pdMS_TO_TICKS(DISPLAY_REFRESH_PERIOD_MS);
        const EventBits_t events =
            xEventGroupWaitBits(s_events, DISPLAY_ALL_EVENTS, pdTRUE, pdFALSE,
                                wait_ticks);
        (void)esp_task_wdt_reset();
        display_heartbeat();
        display_summary_if_due(xTaskGetTickCount());

        portENTER_CRITICAL(&s_health_lock);
        const bool recovery_requested = s_recovery_requested;
        s_recovery_requested = false;
        portEXIT_CRITICAL(&s_health_lock);
        if (recovery_requested) {
            s_animation.abort_requested = true;
            s_force_render_once = true;
        }

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
                startup_elapsed_ms = cycle_elapsed_ms;
                render_startup_frame(startup_elapsed_ms);
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

            render_startup_fade_out(startup_elapsed_ms);
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

        /* 先应用调试接口下发的请求，再处理拨动开关，避免同一次循环互相覆盖。 */
        const int page_request = s_page_request;
        if (page_request >= 0) {
            s_page_request = -1;
            s_page = (display_page_t)page_request;
            APP_LOGI(TAG, "page via http: %d", page_request);
        }
        const int alarm_request = s_alarm_enabled_request;
        if (alarm_request >= 0) {
            s_alarm_enabled_request = -1;
            s_alarm_page.enabled = (alarm_request != 0);
            APP_LOGI(TAG, "alarm via http: %s", s_alarm_page.enabled ? "on" : "off");
        }
        const int volume_request = s_alarm_volume_request;
        if (volume_request > 0) {
            s_alarm_volume_request = -1;
            s_alarm_page.volume = (uint8_t)volume_request;
            APP_LOGI(TAG, "alarm volume via http: %u", (unsigned)s_alarm_page.volume);
        }
        const int voice_request = s_voice_state_request;
        if (voice_request >= 0) {
            s_voice_state_request = -1;
            s_voice_state = (voice_state_t)voice_request;
            /* 重置节拍，让加载动画在切进来的下一轮立即出一帧。 */
            s_spinner_last_us = 0;
            APP_LOGI(TAG, "voice state via http: %s",
                     s_voice_state_name[(unsigned)s_voice_state]);
        }

        /* 页面名用于日志，三页都要列全。 */
        static const char *const k_page_name[DISPLAY_PAGE_COUNT] = {"main", "alarm", "voice"};

        const uint32_t toggles = switch_input_take_toggles();
        if (toggles > 0u) {
            s_page = (display_page_t)((s_page + toggles) % DISPLAY_PAGE_COUNT);
            APP_LOGI(TAG, "page: %s, alarm %02u:%02u %s", k_page_name[(unsigned)s_page],
                     (unsigned)s_alarm_page.hour, (unsigned)s_alarm_page.minute,
                     s_alarm_page.enabled ? "on" : "off");
        }
        if (s_page == DISPLAY_PAGE_ALARM) {
            render_alarm_page();
            continue;
        }
        if (s_page == DISPLAY_PAGE_VOICE) {
            render_voice_page();
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

    /* 预览阶段需要拨动开关切语音表情，失败只告警不影响显示。 */
    if (switch_input_init() != ESP_OK) {
        APP_LOGW(TAG, "switch input init failed, preview switch unavailable");
    }

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
    /* 显示任务卡死时由任务看门狗触发重启，避免画面冻结后必须人工断电。 */
    (void)esp_task_wdt_add(s_display_task_handle);
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
