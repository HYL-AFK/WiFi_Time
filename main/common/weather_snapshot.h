#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    WEATHER_TEMP_SOURCE_NONE = 0,
    WEATHER_TEMP_SOURCE_RTC,
    WEATHER_TEMP_SOURCE_NETWORK,
} weather_temperature_source_t;

typedef struct {
    float temperature_c;
    int humidity;
    int weather_code;
    float wind_kmh;
    bool valid;
    bool temperature_valid;
    weather_temperature_source_t temperature_source;
    time_t updated_at;
} weather_snapshot_t;

esp_err_t weather_snapshot_init(void);
void weather_snapshot_get(weather_snapshot_t *out);
void weather_snapshot_set(const weather_snapshot_t *value);
void weather_snapshot_set_rtc_temperature_q4(int16_t quarter_degrees);
