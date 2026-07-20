#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    UBaseType_t main_stack_free_words;
    TaskHandle_t display_task;
    TaskHandle_t sntp_task;
    TaskHandle_t location_task;
    TaskHandle_t weather_task;
    TaskHandle_t radar_task;
} app_health_task_refs_t;

esp_err_t app_health_init(const app_health_task_refs_t *refs);
