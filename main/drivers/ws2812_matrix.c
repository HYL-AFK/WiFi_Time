#include "ws2812_matrix.h"
#include <string.h>
#include "driver/gpio.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define WS2812_GPIO GPIO_NUM_10
static led_strip_handle_t s_strip;
/* DIN 位于物理右上角；实物索引相对逻辑画面旋转 90 度，逐列同向排列。 */
static matrix_mapping_t s_map = {
    .serpentine = false,
    .mirror_x = false,
    .mirror_y = false,
    .rotation = 3,
};
static uint8_t s_frame[MATRIX_PIXELS][3];
static uint8_t s_displayed_frame[MATRIX_PIXELS][3];
static uint8_t s_displayed_brightness;
static bool s_displayed_frame_valid;

esp_err_t ws2812_matrix_init(void)
{
    led_strip_config_t cfg = {.strip_gpio_num = WS2812_GPIO, .max_leds = MATRIX_PIXELS,
                               .led_pixel_format = LED_PIXEL_FORMAT_GRB,
                               .led_model = LED_MODEL_WS2812,
                               .flags.invert_out = false};
    led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT,
                                   .resolution_hz = 10 * 1000 * 1000,
                                   .flags.with_dma = true};
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) return err;

    s_displayed_frame_valid = false;

    err = gpio_set_pull_mode(WS2812_GPIO, GPIO_FLOATING);
    if (err != ESP_OK) return err;
    return gpio_set_drive_capability(WS2812_GPIO, GPIO_DRIVE_CAP_3);
}

void ws2812_matrix_set_mapping(matrix_mapping_t mapping) { s_map = mapping; }

uint16_t ws2812_matrix_index(uint8_t x, uint8_t y)
{
    if (x >= 16 || y >= 16) return 0;
    if (s_map.mirror_x) x = 15 - x;
    if (s_map.mirror_y) y = 15 - y;
    uint8_t tx = x, ty = y;
    switch (s_map.rotation & 3) {
    case 1: tx = 15 - y; ty = x; break;
    case 2: tx = 15 - x; ty = 15 - y; break;
    case 3: tx = y; ty = 15 - x; break;
    default: break;
    }
    return (uint16_t)(ty * 16 + (s_map.serpentine && (ty & 1) ? 15 - tx : tx));
}

void ws2812_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t i = ws2812_matrix_index(x, y); s_frame[i][0] = r; s_frame[i][1] = g; s_frame[i][2] = b;
}
void ws2812_matrix_clear(void) { memset(s_frame, 0, sizeof(s_frame)); }

esp_err_t ws2812_matrix_show(uint8_t brightness_percent)
{
    if (!s_strip) return ESP_ERR_INVALID_STATE;
    if (brightness_percent > 100) brightness_percent = 100;

    if (s_displayed_frame_valid &&
        s_displayed_brightness == brightness_percent &&
        memcmp(s_frame, s_displayed_frame, sizeof(s_frame)) == 0) {
        return ESP_OK;
    }

    for (int i = 0; i < MATRIX_PIXELS; ++i) {
        uint8_t scale = brightness_percent;
        esp_err_t err = led_strip_set_pixel(s_strip, i,
                                             (s_frame[i][0] * scale) / 100,
                                             (s_frame[i][1] * scale) / 100,
                                             (s_frame[i][2] * scale) / 100);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK) return err;

    memcpy(s_displayed_frame, s_frame, sizeof(s_frame));
    s_displayed_brightness = brightness_percent;
    s_displayed_frame_valid = true;
    return ESP_OK;
}
