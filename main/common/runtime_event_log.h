#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    RUNTIME_EVENT_BOOT = 1,
    RUNTIME_EVENT_WIFI_CONNECTED,
    RUNTIME_EVENT_WIFI_FAILURE,
    RUNTIME_EVENT_SNTP_SYNC,
    RUNTIME_EVENT_WEATHER_SUCCESS,
    RUNTIME_EVENT_WEATHER_FAILURE,
    RUNTIME_EVENT_RADAR_STATE,
    RUNTIME_EVENT_BRIGHTNESS_CHANGE,
    RUNTIME_EVENT_DISPLAY_ERROR,
    RUNTIME_EVENT_DISPLAY_ANIMATION,
} runtime_event_type_t;

/* Display diagnostics use value1/value2 for frame/count and duration/error. */
#define DISPLAY_ANIMATION_BEGIN 1u
#define DISPLAY_ANIMATION_COMPLETE 2u
#define DISPLAY_ANIMATION_ABORT 3u
#define DISPLAY_REFRESH_TIMEOUT (1u << 8)
#define DISPLAY_REFRESH_RETRY (1u << 9)
#define DISPLAY_RMT_RECOVERY_SUCCESS (1u << 10)
#define DISPLAY_RMT_RECOVERY_FAILURE (1u << 11)
#define DISPLAY_HEALTH_STALLED (1u << 12)
#define DISPLAY_SUMMARY (1u << 13)
#define DISPLAY_ANIMATION_SUCCESS_SHIFT 16
#define DISPLAY_ANIMATION_MAX_REFRESH_SHIFT 24
#define DISPLAY_DIAGNOSTIC_ANIMATION_ID_SHIFT 16

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t event_type;
    uint64_t timestamp;
    int32_t value1;
    int32_t value2;
    uint32_t flags;
    uint32_t sequence;
    uint32_t record_crc32;
} runtime_event_record_t;

esp_err_t runtime_event_log_init(void);
esp_err_t runtime_event_log_append(runtime_event_type_t type,
                                   int32_t value1,
                                   int32_t value2,
                                   uint32_t flags);
esp_err_t runtime_event_log_get(uint16_t offset_from_oldest,
                                runtime_event_record_t *out);
uint16_t runtime_event_log_count(void);
esp_err_t runtime_event_log_clear(void);
