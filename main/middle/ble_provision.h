#pragma once

/*
 * BLE provisioning interface based on standard BluFi.
 *
 * The BLE advertising/scan name is the unified device name "ESPARK_XXXXXX",
 * where XXXXXX is the last six hexadecimal digits of the Wi-Fi STA MAC.
 * The SoftAP SSID uses exactly the same string, so BLE and Wi-Fi scans are
 * trivially matched to the same device.
 * Wi-Fi credentials are provisioned through the standard BluFi profile,
 * not the old custom FFF0/FFF1/FFF2 command protocol.
 */

#include <stdint.h>

#include "app_config.h"

/* Called after BLE receives and saves new config, so main.c can re-apply Wi-Fi/AP config. */
typedef void (*ble_cfg_apply_cb_t)(const app_config_t *cfg);

void ble_provision_init(const app_config_t *cfg, ble_cfg_apply_cb_t apply_cb);

/* Push updated Wi-Fi status to BluFi peer after Wi-Fi state changes. */
void ble_provision_refresh_advertising(void);

/* BLE runtime state for UI/status queries. */
bool ble_provision_is_ready(void);
bool ble_provision_is_advertising(void);
bool ble_provision_is_connected(void);
bool ble_provision_is_notify_enabled(void);

/* Current BluFi connection handle; usually 0xFFFF when not connected. */
uint16_t ble_provision_get_conn_handle(void);

/* Current advertising name, derived from the Bluetooth MAC address. */
const char *ble_provision_get_adv_name(void);
