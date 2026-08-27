#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MATRIX_WIDTH 16
#define MATRIX_HEIGHT 16
#define MATRIX_PIXELS 256
typedef struct { bool serpentine; bool mirror_x; bool mirror_y; uint8_t rotation; } matrix_mapping_t;
typedef struct {
    bool grayscale;
    uint8_t x;
    uint8_t y;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint16_t physical_index;
    uint32_t crc32;
} ws2812_frame_diagnostics_t;
typedef struct {
    uint32_t last_refresh_ms;
    uint32_t error_count;
    uint32_t recovery_count;
    esp_err_t last_error;
    bool last_refresh_ok;
} ws2812_refresh_diagnostics_t;
esp_err_t ws2812_matrix_init(void);
void ws2812_matrix_set_mapping(matrix_mapping_t mapping);
uint16_t ws2812_matrix_index(uint8_t x, uint8_t y);
void ws2812_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void ws2812_matrix_clear(void);
esp_err_t ws2812_matrix_show(uint8_t brightness_percent);
esp_err_t ws2812_matrix_show_force(uint8_t brightness_percent);
void ws2812_matrix_get_frame_diagnostics(ws2812_frame_diagnostics_t *out);
void ws2812_matrix_get_refresh_diagnostics(ws2812_refresh_diagnostics_t *out);
