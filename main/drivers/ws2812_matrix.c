#include "ws2812_matrix.h"
#include <string.h>
#include "led_strip.h"
#include "led_strip_rmt.h"

#define WS2812_GPIO GPIO_NUM_10
static led_strip_handle_t s_strip;
static matrix_mapping_t s_map = {.serpentine = true};
static uint8_t s_frame[MATRIX_PIXELS][3];

esp_err_t ws2812_matrix_init(void)
{
    led_strip_config_t cfg = {.strip_gpio_num = WS2812_GPIO, .max_leds = MATRIX_PIXELS,
                               .led_model = LED_MODEL_WS2812, .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
                               .flags.invert_out = false};
    led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000, .flags.with_dma = false};
    return led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
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
    for (int i = 0; i < MATRIX_PIXELS; ++i) {
        uint8_t scale = brightness_percent;
        s_strip->set_pixel(s_strip, i, (s_frame[i][0] * scale) / 100, (s_frame[i][1] * scale) / 100, (s_frame[i][2] * scale) / 100);
    }
    return s_strip->refresh(s_strip);
}
