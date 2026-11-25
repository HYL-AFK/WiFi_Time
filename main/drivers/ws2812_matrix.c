#include "ws2812_matrix.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* SPI 驱动只需要 MOSI 真正接到灯带，SCLK 是内部假时钟，占用 GPIO13（GPIO11/12 已分给 DS3231 I2C）。 */
#define WS2812_GPIO ((gpio_num_t)WS2812_DATA_GPIO)
#define WS2812_SPI_SCLK_GPIO GPIO_NUM_13
#define WS2812_SPI_HOST SPI2_HOST
#define WS2812_SPI_HZ 2500000U
#define WS2812_TX_TIMEOUT_MS 25U
#define WS2812_SYMBOL_BITS 3U
#define WS2812_LATCH_BYTES 100U
#define WS2812_TX_BYTES (MATRIX_PIXELS * 3)
#define WS2812_ENCODED_BYTES (((WS2812_TX_BYTES * 8U * WS2812_SYMBOL_BITS) + 7U) / 8U + WS2812_LATCH_BYTES)
#define WS2812_RECOVERY_BACKOFF_INITIAL_MS 100U
#define WS2812_RECOVERY_BACKOFF_MAX_MS 2000U

static spi_device_handle_t s_spi_device;
static uint8_t *s_tx_buf;
static spi_transaction_t s_transaction;
static bool s_transaction_pending;
static ws2812_refresh_diagnostics_t s_refresh_diag;
static int64_t s_retry_not_before_us;
static uint32_t s_recovery_backoff_ms;
static matrix_mapping_t s_map = {.serpentine = false, .mirror_x = false, .mirror_y = false, .rotation = 1};
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
        for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static void encode_grb_frame(uint8_t brightness_percent)
{
    size_t out = 0;
    memset(s_tx_buf, 0, WS2812_ENCODED_BYTES);
    for (size_t pixel = 0; pixel < MATRIX_PIXELS; ++pixel) {
        const uint8_t grb[3] = {(uint8_t)((s_frame[pixel][1] * brightness_percent) / 100U),
                                (uint8_t)((s_frame[pixel][0] * brightness_percent) / 100U),
                                (uint8_t)((s_frame[pixel][2] * brightness_percent) / 100U)};
        for (size_t byte = 0; byte < 3; ++byte) for (int bit = 7; bit >= 0; --bit) {
            const uint8_t symbol = (grb[byte] & (1U << bit)) ? 0x6U : 0x4U;
            for (int symbol_bit = 2; symbol_bit >= 0; --symbol_bit) {
                if (symbol & (1U << symbol_bit)) s_tx_buf[out / 8U] |= (uint8_t)(1U << (7U - (out % 8U)));
                ++out;
            }
        }
    }
}

static esp_err_t create_spi_resources(void)
{
    const spi_bus_config_t bus_config = {.mosi_io_num = WS2812_GPIO, .miso_io_num = -1,
                                         .sclk_io_num = WS2812_SPI_SCLK_GPIO, .quadwp_io_num = -1,
                                         .quadhd_io_num = -1, .max_transfer_sz = WS2812_ENCODED_BYTES};
    esp_err_t err = spi_bus_initialize(WS2812_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    const spi_device_interface_config_t device_config = {.clock_speed_hz = WS2812_SPI_HZ, .mode = 0,
                                                          .spics_io_num = -1, .queue_size = 1,
                                                          .flags = SPI_DEVICE_HALFDUPLEX};
    err = spi_bus_add_device(WS2812_SPI_HOST, &device_config, &s_spi_device);
    if (err != ESP_OK) { (void)spi_bus_free(WS2812_SPI_HOST); s_spi_device = NULL; }
    return err;
}

static esp_err_t finish_pending_transaction(TickType_t ticks_to_wait)
{
    if (!s_transaction_pending) return ESP_OK;
    spi_transaction_t *returned = NULL;
    esp_err_t err = spi_device_get_trans_result(s_spi_device, &returned, ticks_to_wait);
    if (err == ESP_ERR_TIMEOUT) return err;
    s_transaction_pending = false;
    return (err == ESP_OK && returned == &s_transaction) ? ESP_OK : (err == ESP_OK ? ESP_FAIL : err);
}

static esp_err_t transmit_frame_once(void)
{
    esp_err_t err = finish_pending_transaction(0);
    if (err != ESP_OK) return err;
    memset(&s_transaction, 0, sizeof(s_transaction));
    s_transaction.length = WS2812_ENCODED_BYTES * 8U;
    s_transaction.tx_buffer = s_tx_buf;
    err = spi_device_queue_trans(s_spi_device, &s_transaction, pdMS_TO_TICKS(WS2812_TX_TIMEOUT_MS));
    if (err != ESP_OK) return err;
    s_transaction_pending = true;
    err = finish_pending_transaction(pdMS_TO_TICKS(WS2812_TX_TIMEOUT_MS));
    if (err == ESP_ERR_TIMEOUT) return err;
    esp_rom_delay_us(300);
    return err;
}

static esp_err_t recover_spi_resources(void)
{
    if (s_transaction_pending) return ESP_ERR_TIMEOUT;
    if (s_spi_device != NULL) {
        esp_err_t err = spi_bus_remove_device(s_spi_device);
        if (err != ESP_OK) return err;
        s_spi_device = NULL;
    }
    esp_err_t err = spi_bus_free(WS2812_SPI_HOST);
    if (err != ESP_OK) return err;
    return create_spi_resources();
}

static esp_err_t transmit_frame_with_recovery(void)
{
    if (esp_timer_get_time() < s_retry_not_before_us) return ESP_ERR_TIMEOUT;
    esp_err_t err = transmit_frame_once();
    if (err == ESP_OK) { s_recovery_backoff_ms = 0; s_retry_not_before_us = 0; return ESP_OK; }
    s_refresh_diag.error_count++;
    s_refresh_diag.last_error = err;
    if (err == ESP_ERR_TIMEOUT && s_transaction_pending) return err;
    esp_err_t recovery_err = recover_spi_resources();
    s_refresh_diag.recovery_count++;
    if (recovery_err == ESP_OK) err = transmit_frame_once(); else err = recovery_err;
    if (err != ESP_OK) {
        s_refresh_diag.error_count++;
        s_refresh_diag.last_error = err;
        s_recovery_backoff_ms = s_recovery_backoff_ms == 0 ? WS2812_RECOVERY_BACKOFF_INITIAL_MS : s_recovery_backoff_ms * 2U;
        if (s_recovery_backoff_ms > WS2812_RECOVERY_BACKOFF_MAX_MS) s_recovery_backoff_ms = WS2812_RECOVERY_BACKOFF_MAX_MS;
        s_retry_not_before_us = esp_timer_get_time() + (int64_t)s_recovery_backoff_ms * 1000;
    } else { s_recovery_backoff_ms = 0; s_retry_not_before_us = 0; }
    return err;
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
    s_tx_buf = heap_caps_malloc(WS2812_ENCODED_BYTES, MALLOC_CAP_DMA);
    if (s_tx_buf == NULL) return ESP_ERR_NO_MEM;
    err = create_spi_resources();
    if (err != ESP_OK) { heap_caps_free(s_tx_buf); s_tx_buf = NULL; return err; }
    s_displayed_frame_valid = false;
    return gpio_set_drive_capability(WS2812_GPIO, GPIO_DRIVE_CAP_3);
}

void ws2812_matrix_set_mapping(matrix_mapping_t mapping) { s_map = mapping; }
uint16_t ws2812_matrix_index(uint8_t x, uint8_t y)
{
    if (x >= 16 || y >= 16) return 0;
    if (s_map.mirror_x) x = 15 - x;
    if (s_map.mirror_y) y = 15 - y;
    uint8_t tx = x, ty = y;
    switch (s_map.rotation & 3) { case 1: tx = 15 - y; ty = x; break; case 2: tx = 15 - x; ty = 15 - y; break; case 3: tx = y; ty = 15 - x; break; default: break; }
    return (uint16_t)(ty * 16 + (s_map.serpentine && (ty & 1) ? 15 - tx : tx));
}
void ws2812_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b)
{ if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return; uint16_t i = ws2812_matrix_index(x, y); s_frame[i][0] = r; s_frame[i][1] = g; s_frame[i][2] = b; }
void ws2812_matrix_clear(void) { memset(s_frame, 0, sizeof(s_frame)); }

static esp_err_t show_frame(uint8_t brightness_percent, bool force)
{
    if (s_spi_device == NULL || s_tx_buf == NULL) return ESP_ERR_INVALID_STATE;
    if (brightness_percent > 100) brightness_percent = 100;
    if (!force && s_displayed_frame_valid && s_displayed_brightness == brightness_percent && memcmp(s_frame, s_displayed_frame, sizeof(s_frame)) == 0) return ESP_OK;
    /* Do not overwrite the DMA source buffer until a timed-out transaction is returned. */
    esp_err_t pending_err = finish_pending_transaction(0);
    if (pending_err != ESP_OK) {
        s_refresh_diag.last_refresh_ms = 0;
        s_refresh_diag.last_refresh_ok = false;
        s_refresh_diag.last_error = pending_err;
        return pending_err;
    }
    encode_grb_frame(brightness_percent);
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = transmit_frame_with_recovery();
    s_refresh_diag.last_refresh_ms = (uint32_t)((esp_timer_get_time() - started_us + 999) / 1000);
    s_refresh_diag.last_refresh_ok = err == ESP_OK;
    if (err != ESP_OK) return err;
    memcpy(s_displayed_frame, s_frame, sizeof(s_frame)); s_displayed_brightness = brightness_percent; s_displayed_frame_valid = true;
    return ESP_OK;
}
esp_err_t ws2812_matrix_show(uint8_t brightness_percent) { return show_frame(brightness_percent, false); }
esp_err_t ws2812_matrix_show_force(uint8_t brightness_percent) { return show_frame(brightness_percent, true); }
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
void ws2812_matrix_get_refresh_diagnostics(ws2812_refresh_diagnostics_t *out) { if (out != NULL) *out = s_refresh_diag; }
