#include <stdio.h>
#include "esp_err.h"
#include "nvs_flash.h"
#include "app_log.h"
#include "app_config.h"
#include "app/app_rtc.h"
#include "app/app_time.h"
#include "app/app_weather.h"
#include "app/app_display.h"
#include "wifi_manager.h"
#include "ble_provision.h"

static const char *TAG = "witime";

static void wifi_changed(void)
{
    app_time_notify_network();
    app_weather_notify_network();
    app_display_request_refresh();
}

static void apply_wifi_cfg(const app_config_t *cfg)
{
    wifi_manager_set_ap_name(cfg->ap_name);
    wifi_manager_set_ap_password(cfg->ap_pass);
    if (cfg->has_wifi && cfg->wifi_count > 0) {
        wifi_manager_connect_sta(cfg);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_config_t cfg;
    app_config_load(&cfg);

    ESP_ERROR_CHECK(app_time_init());
    ESP_ERROR_CHECK(app_rtc_init());
    ESP_ERROR_CHECK(app_display_init());
    ESP_ERROR_CHECK(app_weather_init());

    wifi_manager_set_status_cb(wifi_changed);
    wifi_manager_init(&cfg);
    ble_provision_init(&cfg, apply_wifi_cfg);

    if (cfg.has_wifi && cfg.wifi_count > 0) {
        wifi_manager_connect_sta(&cfg);
    } else {
        APP_LOGI(TAG, "no saved Wi-Fi profile; APSTA/BluFi remain available");
    }
}
