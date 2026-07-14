#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MATRIX_WIDTH 16
#define MATRIX_HEIGHT 16
#define MATRIX_PIXELS 256
typedef struct { bool serpentine; bool mirror_x; bool mirror_y; uint8_t rotation; } matrix_mapping_t;
esp_err_t ws2812_matrix_init(void);
void ws2812_matrix_set_mapping(matrix_mapping_t mapping);
uint16_t ws2812_matrix_index(uint8_t x, uint8_t y);
void ws2812_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void ws2812_matrix_clear(void);
esp_err_t ws2812_matrix_show(uint8_t brightness_percent);
