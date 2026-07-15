#include "time_snapshot.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static time_snapshot_t s_value; static SemaphoreHandle_t s_lock;
esp_err_t time_snapshot_init(void) { if (s_lock) return ESP_OK; s_lock = xSemaphoreCreateMutex(); return s_lock ? ESP_OK : ESP_ERR_NO_MEM; }
void time_snapshot_get(time_snapshot_t *out) { if (!out || !s_lock) return; xSemaphoreTake(s_lock, portMAX_DELAY); *out=s_value; xSemaphoreGive(s_lock); }
void time_snapshot_set(const time_snapshot_t *value) { if (!value || !s_lock) return; xSemaphoreTake(s_lock, portMAX_DELAY); s_value=*value; s_value.revision++; xSemaphoreGive(s_lock); }
