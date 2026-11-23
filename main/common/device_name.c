#include "device_name.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_mac.h"

/* 主机名前缀：蓝牙广播名与 WiFi 热点名共用。 */
#define DEVICE_NAME_PREFIX "ESPARK"
#define DEVICE_NAME_MAX_LEN 32

static char s_device_name[DEVICE_NAME_MAX_LEN];
static bool s_device_name_ready;

const char *device_name_get(void)
{
    if (s_device_name_ready) return s_device_name;

    /* 用 WiFi STA MAC（芯片基础 MAC）作为设备标识，保证两种广播名完全一致。 */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_device_name, sizeof(s_device_name), "%s_%02X%02X%02X",
                 DEVICE_NAME_PREFIX, mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_device_name, sizeof(s_device_name), "%s_000000",
                 DEVICE_NAME_PREFIX);
    }
    s_device_name_ready = true;
    return s_device_name;
}
