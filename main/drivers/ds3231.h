#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    struct tm time;
} ds3231_time_t;

typedef struct {
    uint8_t time_regs[7];
    uint8_t status_reg;
    uint32_t revision;
    bool valid;
} ds3231_register_cache_t;

esp_err_t ds3231_init(void);
esp_err_t ds3231_read(ds3231_time_t *out);
esp_err_t ds3231_write(const struct tm *value);
void ds3231_get_register_cache(ds3231_register_cache_t *out);
uint8_t ds3231_bin_to_bcd(uint8_t value);
uint8_t ds3231_bcd_to_bin(uint8_t value);
