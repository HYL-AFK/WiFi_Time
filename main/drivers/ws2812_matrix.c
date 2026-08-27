#include "ws2812_matrix.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define WS2812_GPIO GPIO_NUM_10
#define WS2812_RESOLUTION_HZ 10000000U
#define WS2812_TX_TIMEOUT_MS 25U
#define WS2812_TX_BYTES (MATRIX_PIXELS * 3)
#define WS2812_RECOVERY_BACKOFF_INITIAL_MS 100U
#define WS2812_RECOVERY_BACKOFF_MAX_MS 2000U
static rmt_channel_handle_t s_rmt_channel;
static rmt_encoder_handle_t s_bytes_encoder;
static uint8_t s_tx_buf[WS2812_TX_BYTES];
static ws2812_refresh_diagnostics_t s_refresh_diag;
static int64_t s_retry_not_before_us;
static uint32_t s_recovery_backoff_ms;
/* 在原物理补偿基础上将完整逻辑画面再旋转 180 度，逐列同向排列。 */
static matrix_mapping_t s_map = {
    .serpentine = false,
    .mirror_x = false,
    .mirror_y = false,
    .rotation = 1,
};
static uint8_t s_frame[MATRIX_PIXELS][3];
static uint8_t s_displayed_frame[MATRIX_PIXELS][3];
static uint8_t s_displayed_brightness;
static bool s_displayed_frame_valid;

static uint32_t frame_crc32(void)
{
    uint32_t crc = UINT32_MAX;
    const uint8_t *bytes = &s_frame[0][0];
    for (size_t i = 0; i < sizeof(s_frame); ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static esp_err_t create_rmt_resources(void)
{
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = WS2812_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
        .flags.with_dma = true,
    };
    esp_err_t err = rmt_new_tx_channel(&channel_config, &s_rmt_channel);
    if (err != ESP_OK) return err;

    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&encoder_config, &s_bytes_encoder);
    if (err != ESP_OK) {
        rmt_del_channel(s_rmt_channel);
        s_rmt_channel = NULL;
    }
    return err;
}

static void destroy_rmt_resources(void)
{
    if (s_rmt_channel != NULL) {
        (void)rmt_disable(s_rmt_channel);
    }
    if (s_bytes_encoder != NULL) {
        (void)rmt_del_encoder(s_bytes_encoder);
        s_bytes_encoder = NULL;
    }
    if (s_rmt_channel != NULL) {
        (void)rmt_del_channel(s_rmt_channel);
        s_rmt_channel = NULL;
    }
}

esp_err_t ws2812_matrix_init(void)
{
    esp_err_t err = gpio_set_direction(WS2812_GPIO, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) return err;
    err = gpio_set_pull_mode(WS2812_GPIO, GPIO_PULLDOWN_ONLY);
    if (err != ESP_OK) return err;
    err = gpio_set_level(WS2812_GPIO, 0);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(300);

    err = create_rmt_resources();
    if (err != ESP_OK) return err;

    s_displayed_frame_valid = false;

    err = gpio_set_pull_mode(WS2812_GPIO, GPIO_PULLDOWN_ONLY);
    if (err != ESP_OK) return err;
    return gpio_set_drive_capability(WS2812_GPIO, GPIO_DRIVE_CAP_3);
}

void ws2812_matrix_set_mapping(matrix_mapping_t mapping) { s_map = mapping; }

uint16_t ws2812_matrix_index(uint8_t x, uint8_t y)
{
    /* 先限制逻辑坐标，再按实际点阵方向转换为灯珠索引。 */
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
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;

    uint16_t i = ws2812_matrix_index(x, y);
    s_frame[i][0] = r;
    s_frame[i][1] = g;
    s_frame[i][2] = b;
}
void ws2812_matrix_clear(void) { memset(s_frame, 0, sizeof(s_frame)); }

static esp_err_t transmit_frame_once(void)
{
    esp_err_t err = rmt_enable(s_rmt_channel);
    if (err != ESP_OK) return err;

    const rmt_transmit_config_t tx_config = {.loop_count = 0};
    err = rmt_transmit(s_rmt_channel, s_bytes_encoder, s_tx_buf,
                       sizeof(s_tx_buf), &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_rmt_channel, WS2812_TX_TIMEOUT_MS);
    }
    if (err == ESP_ERR_TIMEOUT) {
        s_refresh_diag.last_error = ESP_ERR_TIMEOUT;
    }
    (void)rmt_disable(s_rmt_channel);
    /* The line is low after transmission; keep it low for the WS2812 latch. */
    esp_rom_delay_us(300);
    return err;
}

static esp_err_t transmit_frame_with_recovery(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_retry_not_before_us) {
        s_refresh_diag.last_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = transmit_frame_once();
    if (err == ESP_OK) {
        s_recovery_backoff_ms = 0;
        s_retry_not_before_us = 0;
        return ESP_OK;
    }

    s_refresh_diag.error_count++;
    s_refresh_diag.last_error = err;
    destroy_rmt_resources();
    esp_err_t recovery_err = create_rmt_resources();
    if (recovery_err != ESP_OK) {
        s_refresh_diag.recovery_count++;
        s_refresh_diag.last_error = recovery_err;
        s_recovery_backoff_ms = s_recovery_backoff_ms == 0
                                    ? WS2812_RECOVERY_BACKOFF_INITIAL_MS
                                    : s_recovery_backoff_ms * 2U;
        if (s_recovery_backoff_ms > WS2812_RECOVERY_BACKOFF_MAX_MS) {
            s_recovery_backoff_ms = WS2812_RECOVERY_BACKOFF_MAX_MS;
        }
        s_retry_not_before_us = esp_timer_get_time() +
                                ((int64_t)s_recovery_backoff_ms * 1000);
        return recovery_err;
    }
    s_refresh_diag.recovery_count++;
    err = transmit_frame_once();
    if (err != ESP_OK) {
        s_refresh_diag.error_count++;
        s_refresh_diag.last_error = err;
        s_recovery_backoff_ms = s_recovery_backoff_ms == 0
                                    ? WS2812_RECOVERY_BACKOFF_INITIAL_MS
                                    : s_recovery_backoff_ms * 2U;
        if (s_recovery_backoff_ms > WS2812_RECOVERY_BACKOFF_MAX_MS) {
            s_recovery_backoff_ms = WS2812_RECOVERY_BACKOFF_MAX_MS;
        }
        s_retry_not_before_us = esp_timer_get_time() +
                                ((int64_t)s_recovery_backoff_ms * 1000);
    } else {
        s_recovery_backoff_ms = 0;
        s_retry_not_before_us = 0;
    }
    return err;
}

static esp_err_t show_frame(uint8_t brightness_percent, bool force)
{
    if (s_rmt_channel == NULL || s_bytes_encoder == NULL) return ESP_ERR_INVALID_STATE;
    if (brightness_percent > 100) brightness_percent = 100;

    if (!force && s_displayed_frame_valid &&
        s_displayed_brightness == brightness_percent &&
        memcmp(s_frame, s_displayed_frame, sizeof(s_frame)) == 0) {
        return ESP_OK;
    }

    for (int i = 0; i < MATRIX_PIXELS; ++i) {
        uint8_t scale = brightness_percent;
        /* The WS2812 wire order is GRB while the logical frame is RGB. */
        s_tx_buf[i * 3 + 0] = (uint8_t)((s_frame[i][1] * scale) / 100);
        s_tx_buf[i * 3 + 1] = (uint8_t)((s_frame[i][0] * scale) / 100);
        s_tx_buf[i * 3 + 2] = (uint8_t)((s_frame[i][2] * scale) / 100);
    }

    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = transmit_frame_with_recovery();
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    s_refresh_diag.last_refresh_ms = (uint32_t)((elapsed_us + 999) / 1000);
    s_refresh_diag.last_refresh_ok = err == ESP_OK;
    if (err != ESP_OK) return err;

    memcpy(s_displayed_frame, s_frame, sizeof(s_frame));
    s_displayed_brightness = brightness_percent;
    s_displayed_frame_valid = true;
    return ESP_OK;
}

esp_err_t ws2812_matrix_show(uint8_t brightness_percent)
{
    return show_frame(brightness_percent, false);
}

esp_err_t ws2812_matrix_show_force(uint8_t brightness_percent)
{
    return show_frame(brightness_percent, true);
}

void ws2812_matrix_get_frame_diagnostics(ws2812_frame_diagnostics_t *out)
{
    if (out == NULL) return;

    memset(out, 0, sizeof(*out));
    out->grayscale = true;
    out->crc32 = frame_crc32();
    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
            const uint16_t index = ws2812_matrix_index(x, y);
            const uint8_t red = s_frame[index][0];
            const uint8_t green = s_frame[index][1];
            const uint8_t blue = s_frame[index][2];
            if (red == green && green == blue) continue;

            out->grayscale = false;
            out->x = x;
            out->y = y;
            out->red = red;
            out->green = green;
            out->blue = blue;
            out->physical_index = index;
            return;
        }
    }
}

void ws2812_matrix_get_refresh_diagnostics(ws2812_refresh_diagnostics_t *out)
{
    if (out == NULL) return;
    *out = s_refresh_diag;
}
