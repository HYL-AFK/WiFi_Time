#include "ds3231.h"
#include <string.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DS3231_PORT I2C_NUM_0
#define DS3231_ADDR 0x68
#define DS3231_SDA GPIO_NUM_11
#define DS3231_SCL GPIO_NUM_12
#define DS3231_I2C_HZ 400000

#define DS3231_TIME_REG 0x00
#define DS3231_STATUS_REG 0x0f
#define DS3231_TIME_REG_COUNT 7
#define DS3231_TEMP_REG 0x11

#define DS3231_HOUR_12H_MODE 0x40
#define DS3231_HOUR_PM 0x20
#define DS3231_HOUR_12H_MASK 0x1f
#define DS3231_HOUR_24H_MASK 0x3f
#define DS3231_MONTH_CENTURY 0x80
#define DS3231_MONTH_MASK 0x1f

static ds3231_register_cache_t s_register_cache;
static SemaphoreHandle_t s_cache_lock;

uint8_t ds3231_bin_to_bcd(uint8_t value) { return (uint8_t)(((value / 10) << 4) | (value % 10)); }
uint8_t ds3231_bcd_to_bin(uint8_t value) { return (uint8_t)(((value >> 4) * 10) + (value & 0x0f)); }

/* 同时兼容芯片可能保留的 12 小时制和项目写入的 24 小时制。 */
static uint8_t decode_hour(uint8_t hour_reg)
{
    if ((hour_reg & DS3231_HOUR_12H_MODE) == 0) {
        return ds3231_bcd_to_bin(hour_reg & DS3231_HOUR_24H_MASK);
    }

    uint8_t hour = ds3231_bcd_to_bin(hour_reg & DS3231_HOUR_12H_MASK);
    if (hour == 12) hour = 0;
    if ((hour_reg & DS3231_HOUR_PM) != 0) hour = (uint8_t)(hour + 12);
    return hour;
}

esp_err_t ds3231_init(void)
{
    static bool initialized;
    if (!s_cache_lock) {
        s_cache_lock = xSemaphoreCreateMutex();
        if (!s_cache_lock) return ESP_ERR_NO_MEM;
    }
    
    if (initialized) return ESP_OK;
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DS3231_SDA,
        .scl_io_num = DS3231_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = DS3231_I2C_HZ,
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

    uint8_t time_regs[DS3231_TIME_REG_COUNT] = {0};
    uint8_t status_reg = 0;
    esp_err_t err = read_regs(DS3231_TIME_REG, time_regs, sizeof(time_regs));
    if (err != ESP_OK) return err;
    err = read_regs(DS3231_STATUS_REG, &status_reg, 1);
    if (err != ESP_OK) return err;

    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    memcpy(s_register_cache.time_regs, time_regs, sizeof(time_regs));
    s_register_cache.status_reg = status_reg;
    s_register_cache.valid = true;
    s_register_cache.revision++;
    xSemaphoreGive(s_cache_lock);

    out->time.tm_sec = ds3231_bcd_to_bin(time_regs[0] & 0x7f);
    out->time.tm_min = ds3231_bcd_to_bin(time_regs[1] & 0x7f);
    out->time.tm_hour = decode_hour(time_regs[2]);
    out->time.tm_wday = ds3231_bcd_to_bin(time_regs[3] & 0x07) - 1;
    out->time.tm_mday = ds3231_bcd_to_bin(time_regs[4] & 0x3f);
    out->time.tm_mon = ds3231_bcd_to_bin(time_regs[5] & DS3231_MONTH_MASK) - 1;
    out->time.tm_year = ds3231_bcd_to_bin(time_regs[6]) + 100;
    if ((time_regs[5] & DS3231_MONTH_CENTURY) != 0) {
        out->time.tm_year += 100;
    }
    return ESP_OK;
}

esp_err_t ds3231_read_temperature_q4(int16_t *out_quarter_degrees)
{
    if (!out_quarter_degrees) return ESP_ERR_INVALID_ARG;

    uint8_t temperature_regs[2] = {0};
    esp_err_t err = read_regs(DS3231_TEMP_REG, temperature_regs,
                               sizeof(temperature_regs));
    if (err != ESP_OK) return err;

    /* MSB 是有符号整数，LSB 的高两位分别代表 0.25/0.50/0.75。 */
    *out_quarter_degrees = (int16_t)(int8_t)temperature_regs[0] * 4 +
                           (int16_t)(temperature_regs[1] >> 6);
    return ESP_OK;
}

void ds3231_get_register_cache(ds3231_register_cache_t *out)
{
    if (!out || !s_cache_lock) return;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY); *out = s_register_cache; xSemaphoreGive(s_cache_lock);
}

esp_err_t ds3231_write(const struct tm *value)
{
    if (!value || value->tm_year < 100 || value->tm_year > 200) return ESP_ERR_INVALID_ARG;

    uint8_t month_reg = ds3231_bin_to_bcd((uint8_t)(value->tm_mon + 1));
    uint8_t year_reg;
    if (value->tm_year >= 200) {
        month_reg |= DS3231_MONTH_CENTURY;
        year_reg = ds3231_bin_to_bcd((uint8_t)(value->tm_year - 200));
    } else {
        year_reg = ds3231_bin_to_bcd((uint8_t)(value->tm_year - 100));
    }

    uint8_t r[8] = {DS3231_TIME_REG,
        ds3231_bin_to_bcd((uint8_t)value->tm_sec), ds3231_bin_to_bcd((uint8_t)value->tm_min),
        ds3231_bin_to_bcd((uint8_t)value->tm_hour), ds3231_bin_to_bcd((uint8_t)(value->tm_wday + 1)),
        ds3231_bin_to_bcd((uint8_t)value->tm_mday), month_reg, year_reg};
    esp_err_t err = i2c_master_write_to_device(DS3231_PORT, DS3231_ADDR, r,
                                                sizeof(r), pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;

    /* 写回成功后同步更新时间寄存器缓存，状态寄存器保持最近一次读取值。 */
    if (s_cache_lock) {
        xSemaphoreTake(s_cache_lock, portMAX_DELAY);
        memcpy(s_register_cache.time_regs, &r[1], DS3231_TIME_REG_COUNT);
        s_register_cache.revision++;
        xSemaphoreGive(s_cache_lock);
    }
    return ESP_OK;
}
