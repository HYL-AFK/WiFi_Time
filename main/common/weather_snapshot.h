#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
typedef struct { float temperature_c; int humidity; int weather_code; float wind_kmh; bool valid; time_t updated_at; } weather_snapshot_t;
esp_err_t weather_snapshot_init(void);
void weather_snapshot_get(weather_snapshot_t *out);
void weather_snapshot_set(const weather_snapshot_t *value);
