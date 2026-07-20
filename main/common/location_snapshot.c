#include "location_snapshot.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static location_snapshot_t s_value;
static SemaphoreHandle_t s_lock;

esp_err_t location_snapshot_init(void)
{
    if (s_lock != NULL) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

bool location_snapshot_get(location_snapshot_t *out)
{
    if (out == NULL || s_lock == NULL) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_value;
    xSemaphoreGive(s_lock);
    return out->valid;
}

esp_err_t location_snapshot_set(double latitude, double longitude,
                                const char *timezone)
{
    if (s_lock == NULL || timezone == NULL || timezone[0] == '\0' ||
        latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_value.latitude = latitude;
    s_value.longitude = longitude;
    strlcpy(s_value.timezone, timezone, sizeof(s_value.timezone));
    s_value.valid = true;
    s_value.revision++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
