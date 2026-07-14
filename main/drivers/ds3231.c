#include "ds3231.h"
#include <string.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DS3231_PORT I2C_NUM_0
#define DS3231_ADDR 0x68
#define DS3231_SDA GPIO_NUM_8
#define DS3231_SCL GPIO_NUM_9

static ds3231_register_cache_t s_register_cache;
static SemaphoreHandle_t s_cache_lock;

uint8_t ds3231_bin_to_bcd(uint8_t value) { return (uint8_t)(((value / 10) << 4) | (value % 10)); }
uint8_t ds3231_bcd_to_bin(uint8_t value) { return (uint8_t)(((value >> 4) * 10) + (value & 0x0f)); }

esp_err_t ds3231_init(void)
{
    static bool initialized;
    if (!s_cache_lock) s_cache_lock = xSemaphoreCreateMutex();
    
    if (initialized) return ESP_OK;
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DS3231_SDA,
        .scl_io_num = DS3231_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err = i2c_param_config(DS3231_PORT, &cfg);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(DS3231_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    initialized = err == ESP_OK;
    return err;
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(DS3231_PORT, DS3231_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

esp_err_t ds3231_read(ds3231_time_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t r[8] = {0};
    esp_err_t err = read_regs(0x00, r, sizeof(r));
    if (err != ESP_OK) return err;
    memset(out, 0, sizeof(*out));
    if (s_cache_lock) { xSemaphoreTake(s_cache_lock, portMAX_DELAY); memcpy(s_register_cache.time_regs, r, 7); s_register_cache.status_reg = r[7]; s_register_cache.valid = true; s_register_cache.revision++; xSemaphoreGive(s_cache_lock); }
    out->time.tm_sec = ds3231_bcd_to_bin(r[0] & 0x7f);
    out->time.tm_min = ds3231_bcd_to_bin(r[1] & 0x7f);
    out->time.tm_hour = ds3231_bcd_to_bin(r[2] & 0x3f);
    out->time.tm_wday = ds3231_bcd_to_bin(r[3] & 0x07) - 1;
    out->time.tm_mday = ds3231_bcd_to_bin(r[4] & 0x3f);
    out->time.tm_mon = ds3231_bcd_to_bin(r[5] & 0x1f) - 1;
    out->time.tm_year = ds3231_bcd_to_bin(r[6]) + 100;
    return ESP_OK;
}

void ds3231_get_register_cache(ds3231_register_cache_t *out)
{
    if (!out || !s_cache_lock) return;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY); *out = s_register_cache; xSemaphoreGive(s_cache_lock);
}

esp_err_t ds3231_write(const struct tm *value)
{
    if (!value || value->tm_year < 100 || value->tm_year > 199) return ESP_ERR_INVALID_ARG;
    uint8_t r[8] = {0x00,
        ds3231_bin_to_bcd((uint8_t)value->tm_sec), ds3231_bin_to_bcd((uint8_t)value->tm_min),
        ds3231_bin_to_bcd((uint8_t)value->tm_hour), ds3231_bin_to_bcd((uint8_t)(value->tm_wday + 1)),
        ds3231_bin_to_bcd((uint8_t)value->tm_mday), ds3231_bin_to_bcd((uint8_t)(value->tm_mon + 1)),
        ds3231_bin_to_bcd((uint8_t)(value->tm_year - 100))};
    return i2c_master_write_to_device(DS3231_PORT, DS3231_ADDR, r, sizeof(r), pdMS_TO_TICKS(100));
}
