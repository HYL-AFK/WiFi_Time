#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    RADAR_PRESENCE_NONE = 0,
    RADAR_PRESENCE_DETECTED,
} radar_presence_state_t;

typedef void (*radar_presence_callback_t)(bool detected, uint32_t distance_mm);

esp_err_t radar_presence_init(void);
void radar_presence_set_callback(radar_presence_callback_t callback);
radar_presence_state_t radar_presence_get_state(void);
bool radar_presence_is_detected(void);
uint32_t radar_presence_get_distance_mm(void);
TaskHandle_t radar_presence_get_task_handle(void);
