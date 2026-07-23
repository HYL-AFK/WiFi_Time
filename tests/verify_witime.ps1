$ErrorActionPreference = 'Stop'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw "FAIL: $message" }
}

function Matrix-Index([int]$x, [int]$y) {
    Assert-True ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) "matrix coordinate out of range"
    return ($x * 16 + (15 - $y))
}

function Seconds-LitCount([int]$seconds) {
    Assert-True ($seconds -ge 0 -and $seconds -lt 60) "seconds out of range"
    return (($seconds % 15) + 1)
}

$versionPath = Join-Path $PSScriptRoot '..\VERSION'
Assert-True (Test-Path $versionPath) "VERSION file is missing"
$projectVersion = (Get-Content -Raw $versionPath).Trim()
Assert-True ($projectVersion -match '^[0-9]+\.[0-9]+\.[0-9]+$') "VERSION must use MAJOR.MINOR.PATCH format"
$rootCmake = Get-Content -Raw (Join-Path $PSScriptRoot '..\CMakeLists.txt')
Assert-True ($rootCmake.Contains('file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER')) "CMake must read the project VERSION file"
Assert-True ($rootCmake.Contains('PROJECT_VER MATCHES')) "CMake must validate the project version"
$packageScript = Join-Path $PSScriptRoot '..\tools\package_firmware.ps1'
Assert-True (Test-Path $packageScript) "firmware package script is missing"

function Ds3231-QuarterDegrees([int]$msb, [int]$lsb) {
    if ($msb -ge 128) { $msb -= 256 }
    return ($msb * 4) + (($lsb -shr 6) -band 0x03)
}

Assert-True ((Matrix-Index 15 0) -eq 255) "rotated upper-right logical pixel must map to the final LED"
Assert-True ((Matrix-Index 15 15) -eq 240) "rotated lower-right edge must remain one physical column"
Assert-True ((Matrix-Index 0 0) -eq 15) "rotated upper-left logical pixel must map to the first physical column"
Assert-True ((Matrix-Index 0 15) -eq 0) "rotated lower-left logical pixel must map to DIN LED 0"
for ($y = 0; $y -lt 16; ++$y) {
    Assert-True ((Matrix-Index 15 $y) -eq (255 - $y)) "rotated right edge must map to descending consecutive LEDs"
}
Assert-True ((Seconds-LitCount 0) -eq 1) "seconds bar must restart with one pixel"
Assert-True ((Seconds-LitCount 14) -eq 15) "seconds bar must fill at second 14"
Assert-True ((Seconds-LitCount 15) -eq 1) "seconds bar must restart at second 15"
Assert-True ((Seconds-LitCount 29) -eq 15) "seconds bar must fill at second 29"
Assert-True ((Seconds-LitCount 44) -eq 15) "seconds bar must fill at second 44"
Assert-True ((Seconds-LitCount 59) -eq 15) "seconds bar must fill at second 59"

$required = @(
    'main/drivers/ds3231.c', 'main/drivers/ds3231.h',
    'main/drivers/ws2812_matrix.c', 'main/drivers/ws2812_matrix.h',
    'main/APP/app_rtc.c', 'main/APP/app_time.c',
    'main/APP/app_weather.c', 'main/APP/app_display.c',
    'main/common/location_snapshot.c', 'main/common/location_snapshot.h',
    'D:\Project\WiTime_Hardware\witime_design.drawio'
)
foreach ($path in $required) { Assert-True (Test-Path $path) "missing $path" }
$pinoutPath = Join-Path 'D:\Project\WiTime_Hardware' (([char]0x5f15) + ([char]0x811a) + ([char]0x56fe) + '.drawio')
Assert-True (Test-Path $pinoutPath) "missing user pinout file"

$rtcSource = Get-Content -Raw main/APP/app_rtc.c
$displaySource = Get-Content -Raw main/APP/app_display.c
$displayHeader = Get-Content -Raw main/APP/app_display.h
$timeSource = Get-Content -Raw main/APP/app_time.c
$timeHeader = Get-Content -Raw main/APP/app_time.h
$weatherSource = Get-Content -Raw main/APP/app_weather.c
$ws2812Source = Get-Content -Raw main/drivers/ws2812_matrix.c
$ws2812Header = Get-Content -Raw main/drivers/ws2812_matrix.h
$wifiSource = Get-Content -Raw main/drivers/wifi_manager.c
$bleProvisionSource = Get-Content -Raw main/middle/ble_provision.c
$weatherSnapshotHeader = Get-Content -Raw main/common/weather_snapshot.h
$weatherSnapshotSource = Get-Content -Raw main/common/weather_snapshot.c
$sdkconfig = Get-Content -Raw sdkconfig
$sdkconfigDefaults = Get-Content -Raw sdkconfig.defaults
$blufiSecuritySource = Get-Content -Raw main/middle/blufi_security.c
$mainCmake = Get-Content -Raw main/CMakeLists.txt
$mainSource = Get-Content -Raw main/main.c
$timeSnapshotHeader = Get-Content -Raw main/common/time_snapshot.h
$ds3231Source = Get-Content -Raw main/drivers/ds3231.c
$ds3231Header = Get-Content -Raw main/drivers/ds3231.h
$locationSource = Get-Content -Raw main/common/location_snapshot.c
$locationHeader = Get-Content -Raw main/common/location_snapshot.h
$configSource = Get-Content -Raw main/common/app_config.c
$radarSource = Get-Content -Raw main/drivers/radar_presence.c
$radarHeader = Get-Content -Raw main/drivers/radar_presence.h
$configHeader = Get-Content -Raw main/common/app_config.h
$appLogHeader = Get-Content -Raw main/common/app_log.h
$runtimeLogSource = Get-Content -Raw main/common/runtime_event_log.c
$runtimeLogHeader = Get-Content -Raw main/common/runtime_event_log.h

Assert-True (-not $rtcSource.Contains('value.valid')) "RTC task must not validate RTC time"
Assert-True (-not $displaySource.Contains('t.valid')) "display task must not gate RTC display on valid"
Assert-True (-not $timeSource.Contains('.valid=true')) "SNTP snapshot must not use removed time valid flag"
Assert-True (-not $timeSnapshotHeader.Contains('bool valid')) "time snapshot must not expose RTC validity"
Assert-True ($timeSnapshotHeader.Contains('bool time_snapshot_is_ready(void)')) "time snapshot readiness API is missing"
Assert-True ($runtimeLogSource.Contains('#define RUNTIME_EVENT_LOG_CAPACITY 16') -and
             $runtimeLogSource.Contains('#define RUNTIME_EVENT_RECORD_SIZE 32')) "runtime event log size is not fixed at 16x32 bytes"
Assert-True ($runtimeLogSource.Contains('write_index') -and
             $runtimeLogSource.Contains('read_index') -and
             $runtimeLogSource.Contains('count')) "runtime event log ring pointers are missing"
Assert-True ($runtimeLogSource.Contains('timestamp') -and
             $runtimeLogSource.Contains('nvs_set_blob') -and
             $runtimeLogSource.Contains('record_crc32')) "runtime event log must persist timestamped CRC records"
Assert-True ($runtimeLogHeader.Contains('runtime_event_log_append') -and
             $runtimeLogHeader.Contains('runtime_event_log_get')) "runtime event log API is missing"
Assert-True ((Get-Content -Raw main/CMakeLists.txt).Contains('common/runtime_event_log.c')) "runtime event log must be part of the component"
Assert-True ($mainSource.Contains('runtime_event_log_init') -and
             $mainSource.Contains('RUNTIME_EVENT_BOOT')) "boot event must be stored"
Assert-True ($wifiSource.Contains('RUNTIME_EVENT_WIFI_CONNECTED') -and
             $wifiSource.Contains('RUNTIME_EVENT_WIFI_FAILURE')) "Wi-Fi events must be stored"
Assert-True ($timeSource.Contains('RUNTIME_EVENT_SNTP_SYNC')) "SNTP event must be stored"
Assert-True ($weatherSource.Contains('RUNTIME_EVENT_WEATHER_SUCCESS') -and
             $weatherSource.Contains('RUNTIME_EVENT_WEATHER_FAILURE')) "weather events must be stored"
Assert-True ((Get-Content -Raw main/common/time_snapshot.c).Contains('s_time_ready')) "time snapshot must track readiness independently"
Assert-True (-not $ds3231Source.Contains('out->valid')) "DS3231 driver must not calculate time validity"
Assert-True ($locationHeader.Contains('location_snapshot_t')) "location snapshot type is missing"
Assert-True ($locationHeader.Contains('latitude') -and $locationHeader.Contains('longitude')) "location snapshot must store coordinates"
Assert-True ($locationSource.Contains('location_snapshot_set') -and $locationSource.Contains('location_snapshot_get')) "location snapshot accessors are missing"
Assert-True (-not $configHeader.Contains('has_cached_location') -and
             -not $configHeader.Contains('cached_latitude_e6') -and
             -not $configHeader.Contains('cached_longitude_e6') -and
             -not $configHeader.Contains('cached_timezone') -and
             -not $configHeader.Contains('location_cache_version')) "application config must not retain location cache fields"
Assert-True (-not $configHeader.Contains('app_config_save_location_cache') -and
             -not $configHeader.Contains('app_config_clear_location_cache')) "location cache persistence APIs must be removed"
Assert-True ($configSource.Contains('erase_legacy_location_keys')) "firmware upgrades must erase legacy location cache keys"
Assert-True (-not $timeSource.Contains('app_config_save_location_cache') -and
             -not $timeSource.Contains('has_cached_location')) "time service must not read or persist location in NVS"
Assert-True ($timeSource.Contains('https://ipwho.is/')) "time task must use the primary IP geolocation API"
Assert-True ($timeSource.Contains('https://ipapi.co/json/')) "time task must retain a fallback geolocation API"
Assert-True (-not $timeSource.Contains('https://ipapi.co/timezone/')) "time task must not use the timezone-only API"
Assert-True ($timeSource.Contains('esp_http_client_set_header') -and $timeSource.Contains('User-Agent')) "location requests must identify the firmware client"
Assert-True ($timeSource.Contains('cJSON_GetObjectItem') -and $timeSource.Contains('latitude') -and $timeSource.Contains('longitude')) "time task must parse geolocation coordinates"
Assert-True ($timeSource.Contains('timezone') -and $timeSource.Contains('id')) "time task must parse nested and flat timezone responses"
Assert-True ($timeSource.Contains('HTTP %d')) "location HTTP status failures must be logged"
Assert-True ($weatherSource.Contains('location_snapshot_get')) "weather task must use the dynamic location snapshot"
Assert-True (-not $weatherSource.Contains('latitude=31.23&longitude=121.47')) "weather URL must not hard-code Shanghai coordinates"
Assert-True ($weatherSource.Contains('snprintf') -and $weatherSource.Contains('%.6f')) "weather URL must be built from the location snapshot"
Assert-True ($weatherSource.Contains('current=temperature_2m,weather_code')) "weather request must only fetch temperature and condition"
Assert-True (-not $weatherSource.Contains('relative_humidity_2m')) "weather request must not fetch humidity"
Assert-True (-not $weatherSource.Contains('wind_speed_10m')) "weather request must not fetch wind speed"
Assert-True (-not $weatherSnapshotHeader.Contains('humidity') -and
             -not $weatherSnapshotHeader.Contains('wind_kmh')) "weather snapshot must only keep displayed network fields"
Assert-True ($rtcSource.Contains('s_rtc_time_loaded')) "RTC task must track one-time system clock initialization"
Assert-True ($rtcSource.Contains('s_ds3231_ready')) "RTC task must track DS3231 initialization readiness"
Assert-True ($rtcSource.Contains('if (!s_ds3231_ready)') -and
             $rtcSource.Contains('ds3231_init()')) "RTC task must retry non-fatal DS3231 initialization"
Assert-True (-not $rtcSource.Contains('settimeofday(&tv, NULL);') -or
             ($rtcSource.Contains('s_ds3231_ready && !s_rtc_time_loaded') -and
              $rtcSource.Contains('s_rtc_time_loaded = true'))) "RTC system time must not be reset every second"
Assert-True ($ws2812Source.Contains('if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT)')) "WS2812 pixel writes must reject out-of-range coordinates"
Assert-True ($weatherSource.Contains('s_body_overflow') -and $weatherSource.Contains('response overflow')) "weather HTTP response overflow must be detected"
Assert-True ($timeSource.Contains('s_location_body_overflow') -and $timeSource.Contains('location response overflow')) "location HTTP response overflow must be detected"
Assert-True ($configSource.Contains('nvs_commit') -and $configSource.Contains('APP_LOGE') -and $configSource.Contains('nvs_set_str')) "NVS save errors must be logged through app_log"
Assert-True ($configHeader.Contains('esp_err_t app_config_save')) "NVS save API must return an error code"
Assert-True ($configHeader.Contains('last_wifi_ssid') -and
             $configHeader.Contains('has_last_wifi_ssid')) "last successful Wi-Fi metadata is missing"
Assert-True ($configHeader.Contains('app_config_save_last_wifi_ssid')) "targeted last SSID save API is missing"
Assert-True ($configSource.Contains('"last_ssid"')) "last successful SSID NVS key is missing"
$configSaveStart = $configSource.IndexOf('esp_err_t app_config_save(const app_config_t *cfg)')
$lastSsidSaveStart = $configSource.IndexOf('esp_err_t app_config_save_last_wifi_ssid')
Assert-True ($configSaveStart -ge 0 -and $lastSsidSaveStart -gt $configSaveStart) "config save functions are malformed"
$fullConfigSave = $configSource.Substring($configSaveStart, $lastSsidSaveStart - $configSaveStart)
Assert-True (-not $fullConfigSave.Contains('"last_ssid"')) "full config save must not overwrite runtime last SSID metadata"
Assert-True ($wifiSource.Contains('esp_err_t wifi_manager_init')) "Wi-Fi initialization must return an error code"
Assert-True (-not $wifiSource.Contains('ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP')) "runtime AP config must not abort the firmware"
Assert-True (-not $blufiSecuritySource.Contains('APP_LOG_HEX')) "BluFi security key logging must be removed"
$timeResult = [regex]::Match($ds3231Header, '(?s)typedef struct \{.*?\} ds3231_time_t;').Value
Assert-True ($timeResult.Length -gt 0) "missing DS3231 time result type"
Assert-True (-not $timeResult.Contains('bool valid;')) "DS3231 time result must not expose time validity"
Assert-True ($ds3231Source.Contains('#define DS3231_STATUS_REG 0x0f')) "DS3231 status register must use address 0x0F"
Assert-True ($ds3231Source.Contains('read_regs(DS3231_STATUS_REG, &status_reg, 1)')) "DS3231 status register must be read separately"
Assert-True (-not $ds3231Source.Contains('status_reg = r[7]')) "alarm register 0x07 must not be cached as status"
Assert-True ($ds3231Source.Contains('#define DS3231_I2C_HZ 400000')) "DS3231 I2C bus must run at 400kHz"
Assert-True ($radarSource.Contains('#define RADAR_PRESENCE_GPIO GPIO_NUM_13')) "radar presence must use GPIO13"
Assert-True ($radarSource.Contains('#define RADAR_UART_BAUD_RATE 256000')) "LD2410C UART baud rate must be 256000"
Assert-True ($radarSource.Contains('UART_NUM_1') -and
             $radarSource.Contains('GPIO_NUM_1') -and
             $radarSource.Contains('GPIO_NUM_2')) "radar UART pins must use the board TX/RX pins"
Assert-True ($radarSource.Contains('0xf4') -and $radarSource.Contains('0xf3') -and
             $radarSource.Contains('0xf2') -and $radarSource.Contains('0xf1') -and
             $radarSource.Contains('0xf8') -and $radarSource.Contains('0xf7') -and
             $radarSource.Contains('0xf6') -and $radarSource.Contains('0xf5')) "LD2410C data frame markers are missing"
Assert-True ($radarSource.Contains('#define RADAR_DISTANCE_THRESHOLD_MM 500')) "radar distance threshold must be 500mm"
Assert-True ($radarSource.Contains('uart_read_bytes') -and
             $radarSource.Contains('uart_driver_install')) "radar UART task is missing"
Assert-True ($radarSource.Contains('s_set_resolution') -and
             $radarSource.Contains('0xaa') -and $radarSource.Contains('0x01')) "radar must request 0.2m resolution"
Assert-True ($radarHeader.Contains('radar_presence_get_distance_mm') -and
             $radarHeader.Contains('radar_presence_set_callback')) "radar distance callback API is missing"
Assert-True ($radarSource.Contains('#define RADAR_SAMPLE_PERIOD_MS 50')) "radar UART polling period must be 50ms"
Assert-True ($radarSource.Contains('GPIO_PULLUP_DISABLE') -and
             $radarSource.Contains('GPIO_PULLDOWN_DISABLE')) "radar input must not force a pull resistor"
Assert-True ($radarSource.Contains('uart_read_bytes') -and
             $radarSource.Contains('xTaskCreate')) "radar driver must receive asynchronously"
Assert-True ($radarSource.Contains('RADAR_PRESENCE_DETECTED') -and
             $radarSource.Contains('RADAR_PRESENCE_NONE')) "radar driver presence states are missing"
Assert-True ($radarHeader.Contains('radar_presence_init') -and
             $radarHeader.Contains('radar_presence_is_detected')) "radar presence public API is missing"
Assert-True ((Get-Content -Raw main/CMakeLists.txt).Contains('drivers/radar_presence.c')) "radar driver must be part of the component"
Assert-True ((Get-Content -Raw main/main.c).Contains('radar_presence_init')) "main must initialize the radar driver"
Assert-True ($displaySource.Contains('#define MATRIX_BRIGHTNESS_HIGH 60') -and
             $displaySource.Contains('#define MATRIX_BRIGHTNESS_LOW 20')) "display high/low brightness levels are missing"
Assert-True ($displayHeader.Contains('app_display_set_brightness')) "display brightness setter is missing"
Assert-True ($displaySource.Contains('s_brightness_percent')) "display must use runtime brightness"
Assert-True ($mainSource.Contains('radar_presence_set_callback') -and
             $mainSource.Contains('app_display_set_brightness')) "main must connect radar state to display brightness"
Assert-True ($mainSource.Contains('WITIME_RADAR_ENABLED 1') -and
             $mainSource.Contains('WITIME_RADAR_STANDALONE_TEST 0') -and
             $mainSource.Contains('#if WITIME_RADAR_ENABLED')) "normal clock firmware must keep radar optional"
Assert-True ($mainSource.Contains('esp_reset_reason') -and
             $mainSource.Contains('esp_get_free_heap_size')) "boot reset and heap diagnostics are missing"
Assert-True ($mainSource.Contains('app_health_init')) "health monitor must be initialized"
Assert-True ((Get-Content -Raw main/APP/app_health.c).Contains('uxTaskGetStackHighWaterMark') -and
             (Get-Content -Raw main/APP/app_health.c).Contains('temperature_sensor_get_celsius')) "health monitor must report stack and chip temperature"
Assert-True ((Get-Content -Raw main/APP/app_health.c).Contains('esp_task_wdt_status')) "health monitor must report task watchdog status"
Assert-True ((Get-Content -Raw main/APP/app_health.h).Contains('app_health_init')) "health monitor API is missing"
Assert-True ($displaySource.Contains('DISPLAY_REFRESH_WARN_US') -and
             $displaySource.Contains('esp_timer_get_time')) "WS2812 refresh timing diagnostics are missing"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_ESP_COREDUMP_ENABLE_TO_UART=y') -and
             $sdkconfigDefaults.Contains('CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y')) "UART coredump defaults are missing"
Assert-True ($ds3231Source.Contains('DS3231_MONTH_CENTURY')) "DS3231 century bit handling is missing"
Assert-True ($ds3231Source.Contains('value->tm_year > 200')) "DS3231 write range must include year 2100"
Assert-True ($ds3231Source.Contains('DS3231_HOUR_12H_MODE')) "DS3231 12-hour register decoding is missing"
Assert-True ((Ds3231-QuarterDegrees 0x19 0x40) -eq 101) "DS3231 positive temperature decode is incorrect"
Assert-True ((Ds3231-QuarterDegrees 0xff 0xc0) -eq -1) "DS3231 negative temperature decode is incorrect"
Assert-True ((Ds3231-QuarterDegrees 0xe6 0xc0) -eq -101) "DS3231 negative fractional temperature decode is incorrect"
Assert-True ($ds3231Header.Contains('ds3231_read_temperature_q4')) "missing DS3231 temperature API"
Assert-True ($ds3231Source.Contains('DS3231_TEMP_REG 0x11')) "missing DS3231 temperature register address"
Assert-True ($ds3231Source.Contains('temperature_regs[1] >> 6')) "missing DS3231 temperature fractional decode"
Assert-True ($weatherSnapshotHeader.Contains('temperature_valid')) "weather snapshot must separate temperature validity"
Assert-True ($weatherSnapshotHeader.Contains('temperature_source')) "weather snapshot must track temperature source"
Assert-True ($weatherSnapshotSource.Contains('weather_snapshot_set_rtc_temperature_q4')) "missing RTC temperature snapshot setter"
Assert-True ($rtcSource.Contains('s_rtc_temperature_loaded')) "RTC task must retry temperature until first success"
Assert-True ($rtcSource.Contains('ds3231_read_temperature_q4')) "RTC task must read DS3231 temperature"
Assert-True ($rtcSource.Contains('static const char *TAG = "app_rtc"')) "RTC app must define a log tag"
Assert-True ($rtcSource.Contains('RTC time read OK')) "RTC time success log is missing"
Assert-True ($rtcSource.Contains('RTC time read failed')) "RTC time failure log is missing"
Assert-True ($rtcSource.Contains('RTC time read recovered')) "RTC time recovery log is missing"
Assert-True ($rtcSource.Contains('RTC temperature read OK')) "RTC temperature success log is missing"
Assert-True ($rtcSource.Contains('RTC temperature read failed')) "RTC temperature failure log is missing"
Assert-True ($rtcSource.Contains('RTC writeback OK')) "RTC SNTP writeback success log is missing"
Assert-True ($rtcSource.Contains('status=0x%02x') -and $rtcSource.Contains('cache_rev=%')) "RTC register diagnostics are missing"
Assert-True ($weatherSource.Contains('WEATHER_TEMP_SOURCE_NETWORK')) "network weather must mark temperature source"
Assert-True ($displaySource.Contains('weather->temperature_valid')) "display must show RTC temperature without full weather"
Assert-True ($timeSource.Contains('#include "esp_crt_bundle.h"')) "time HTTPS client must include certificate bundle"
Assert-True ($timeSource.Contains('.crt_bundle_attach = esp_crt_bundle_attach')) "time HTTPS client must attach certificate bundle"
Assert-True ($timeSource.Contains('Asia/Shanghai')) "timezone response must recognize Asia/Shanghai"
Assert-True ($timeSource.Contains('setenv("TZ", "CST-8", 1)')) "China timezone must use ESP-IDF compatible CST-8"
Assert-True (-not $timeSource.Contains('setenv("TZ", s_timezone, 1)')) "IANA timezone must not be assigned directly to TZ"
Assert-True ($timeSource.Contains('static bool s_sntp_initialized')) "SNTP initialization state is missing"
Assert-True (-not $timeSource.Contains('esp_netif_sntp_start()')) "default SNTP config must not be started twice"
Assert-True (-not $timeSource.Contains('sntp_started=false')) "SNTP initialized state must not reset after sync"
Assert-True ($weatherSource.Contains('#include "esp_crt_bundle.h"')) "weather HTTPS client must include certificate bundle"
Assert-True ($weatherSource.Contains('.crt_bundle_attach = esp_crt_bundle_attach')) "weather HTTPS client must attach certificate bundle"
Assert-True ($weatherSource.Contains('weather_condition_name')) "weather logs must expose a readable condition name"
Assert-True ($weatherSource.Contains('weather code=%d condition=%s')) "weather success log must include code and condition"
Assert-True ($weatherSource.Contains('return "cloudy"')) "weather condition names must include cloudy"
Assert-True ($weatherSource.Contains('if (!wifi_manager_is_sta_connected())')) "periodic weather refresh must stay idle while STA has no IP"
$temperatureRenderer = [regex]::Match($displaySource, '(?s)static void draw_temperature\(.*?\n\}')
Assert-True ($temperatureRenderer.Success) "missing temperature renderer"
$degreeOffset = $temperatureRenderer.Value.IndexOf('set_rect(DEGREE_X')
Assert-True ($degreeOffset -ge 0) "degree symbol must display with RTC or network temperature"
Assert-True (-not $temperatureRenderer.Value.Contains('weather->valid')) "temperature renderer must not require full network weather"

Assert-True ($displaySource.Contains('#define CLOCK_W 13')) "missing 13-column clock layout"
Assert-True ($displaySource.Contains('#define CLOCK_H 5')) "missing 5-row clock layout"
Assert-True ($displaySource.Contains('#define WEATHER_X 0') -and $displaySource.Contains('#define WEATHER_Y 10')) "weather icon coordinates are incorrect"
Assert-True ($displaySource.Contains('#define WEATHER_W 5') -and $displaySource.Contains('#define WEATHER_H 5')) "weather icon must use 5x5 pixels"
Assert-True ($displaySource.Contains('s_weather_cloud[WEATHER_H] = {0x00, 0x0e, 0x1f, 0x1f, 0x00}')) "cloud icon must leave its last row empty"
Assert-True ($displaySource.Contains('s_weather_thunder')) "thunderstorm must have a distinct icon"
Assert-True ($displaySource.Contains('weather_code >= 1 && weather_code <= 3')) "weather codes 1..3 must map to cloudy"
Assert-True ($displaySource.Contains('weather_code == 45 || weather_code == 48')) "fog codes must map to cloudy"
Assert-True ($displaySource.Contains('weather_code >= 80 && weather_code <= 82')) "rain codes 80..82 must map to rain"
Assert-True ($displaySource.Contains('weather_code >= 95 && weather_code <= 99')) "thunder codes 95..99 must map to thunder"
Assert-True ($displaySource.Contains('draw_weather_placeholder') -and
             $displaySource.Contains('if (!weather->valid)')) "missing weather data must render a placeholder"
Assert-True ($displaySource.Contains('WEATHER_PLACEHOLDER_Y') -and
             $displaySource.Contains('set_rect(WEATHER_X, WEATHER_PLACEHOLDER_Y, 2, 1') -and
             $displaySource.Contains('set_rect(WEATHER_X + 3, WEATHER_PLACEHOLDER_Y, 2, 1')) "weather placeholder must render two dashes"
Assert-True ($displaySource.Contains('#define CLOCK_HOUR_TENS_X 0')) "clock hour tens coordinate is incorrect"
Assert-True ($displaySource.Contains('#define CLOCK_HOUR_ONES_X 3')) "clock hour ones coordinate is incorrect"
Assert-True ($displaySource.Contains('#define CLOCK_COLON_X 6')) "clock colon coordinate is incorrect"
Assert-True ($displaySource.Contains('#define CLOCK_MINUTE_TENS_X 7')) "clock minute tens must move left one column"
Assert-True ($displaySource.Contains('#define CLOCK_MINUTE_ONES_X 10')) "clock minute ones must move left one column"
Assert-True ($displaySource.Contains('#define TEMP_TENS_X 7') -and $displaySource.Contains('#define TEMP_ONES_X 10')) "temperature digit coordinates are incorrect"
Assert-True ($displaySource.Contains('#define TEMP_Y 10')) "temperature must move up one row"
Assert-True ($displaySource.Contains('#define NEGATIVE_X 6') -and $displaySource.Contains('#define NEGATIVE_Y 12')) "negative temperature marker must move up one row"
Assert-True ($displaySource.Contains('#define DEGREE_X 13') -and $displaySource.Contains('#define DEGREE_Y 10')) "degree symbol must move down one row"
Assert-True (-not $displaySource.Contains('set_rect(0, 6, 13, 4')) "middle five rows must remain off"
Assert-True ($displaySource.Contains('s_digit_3x5')) "clock must use a readable 3x5 digit font"
Assert-True ($displaySource.Contains('draw_digit_3x5')) "missing 3x5 clock digit renderer"
Assert-True ($displaySource.Contains('draw_digit_3x5(CLOCK_HOUR_TENS_X')) "clock must use the 3x5 font"
Assert-True ($displaySource.Contains('draw_digit_3x5(TEMP_TENS_X')) "temperature must use the 3x5 font"
Assert-True ($displaySource.Contains('{0x02, 0x02, 0x02, 0x02, 0x02}')) "digit 1 must be centered in the 3x5 grid"
Assert-True ($displaySource.Contains('{0x07, 0x01, 0x01, 0x01, 0x01}')) "digit 7 must use the seven-segment Arabic style"
Assert-True ($displaySource.Contains('{0x07, 0x05, 0x07, 0x01, 0x07}')) "digit 9 must use the seven-segment Arabic style"
Assert-True (-not $displaySource.Contains('s_digit_2x5')) "display must not retain the unused 2x5 digit font"
Assert-True (-not $displaySource.Contains('draw_digit_2x5')) "display must not retain the unused 2x5 digit renderer"
Assert-True ($displaySource.Contains('#define DISPLAY_WHITE')) "display must define one white color level"
Assert-True ($displaySource.Contains('#define MATRIX_BRIGHTNESS_HIGH 60') -and
             $displaySource.Contains('#define MATRIX_BRIGHTNESS_LOW 20')) "matrix brightness levels must be 60/20 percent"
Assert-True ($displaySource.Contains('DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE')) "time, weather, temperature and seconds must use white LEDs"
Assert-True ($displaySource.Contains('local_time->tm_sec') -and
             $displaySource.Contains('CLOCK_COLON_X')) "clock colon must blink from the current second"
Assert-True ($displaySource.Contains('#define STARTUP_FRAME_PERIOD_MS 50')) "startup chase animation must refresh smoothly every 50ms"
Assert-True ($displaySource.Contains('static uint32_t startup_frame_for_elapsed')) "startup chase must derive its position from elapsed time"
Assert-True ($displaySource.Contains('STARTUP_RING_LAP_DURATION_MS 6000')) "one startup ring lap must last 6 seconds"
Assert-True (-not $displaySource.Contains('render_startup_frame(startup_frame++)')) "startup ring must not restart from a counter increment at cycle boundaries"
Assert-True ($displaySource.Contains('#define STARTUP_RING_TRAIL_LENGTH 5') -and
             $displaySource.Contains('s_startup_trail_levels')) "startup ring chase must use a five-pixel fading trail"
Assert-True (-not $displaySource.Contains('STARTUP_HAND_FRAME') -and
             -not $displaySource.Contains('s_startup_hand')) "startup animation must not retain the clock hand"
Assert-True (-not $displaySource.Contains('set_rect(7, 7, 2, 2')) "startup animation must not retain the center block"
Assert-True ($displaySource.Contains('render_startup_frame_scaled') -and
             -not $displaySource.Contains('DISPLAY_CYAN_R')) "startup animation must use grayscale brightness levels"
Assert-True ($displaySource.Contains('#define STARTUP_FADE_STEP_MS 100') -and
             $displaySource.Contains('render_startup_fade_out(startup_frame)')) "startup page must use a two-step 200ms fade before transitions"
Assert-True ($displaySource.Contains('#define STARTUP_RING_LAP_DURATION_MS 6000')) "startup ring lap must be 6 seconds"
Assert-True ($displaySource.Contains('#define BOOT_ANIMATION_CYCLE_COUNT 3')) "boot must allow three animation cycles before failure"
Assert-True ($displaySource.Contains('boot_animation_cycle')) "boot animation cycle counter is missing"
Assert-True ($displaySource.Contains('boot_animation_cycle >= BOOT_ANIMATION_CYCLE_COUNT')) "boot failure must wait for three complete animation cycles"
Assert-True (-not $displaySource.Contains('BOOT_TIME_FALLBACK_DEADLINE_MS')) "boot must not use the old absolute time failure deadline"
Assert-True (-not $displaySource.Contains('BOOT_NETWORK_DEADLINE_MS')) "boot must not wait 15 seconds for a network result"
Assert-True (-not $displaySource.Contains('boot_time_failure_shown')) "boot failure must not be triggered only by missing time"
Assert-True ($displaySource.Contains('DISPLAY_MODE_BOOT') -and
             $displaySource.Contains('DISPLAY_MODE_MAIN')) "display boot/main state machine is missing"
Assert-True ($displaySource.Contains('static void set_elapsed_time')) "display must derive a 00:00 fallback clock when no time source is ready"
Assert-True ($displaySource.Contains('elapsed_seconds')) "fallback clock must advance from display uptime"
Assert-True ($displaySource.Contains('boot_timed_out_without_time_source')) "display must explicitly handle boot timeout without a time source"
Assert-True ($displaySource -match '(?s)boot_timed_out_without_time_source.*?mode = DISPLAY_MODE_MAIN;') "boot timeout without RTC or Wi-Fi must enter the main display"
Assert-True ($displaySource -match '(?s)NETWORK_NOTICE_SUCCESS.*?mode = DISPLAY_MODE_MAIN;') "Wi-Fi success must enter the main display without waiting for SNTP"
Assert-True ($displaySource.Contains('if (pending_notice == NETWORK_NOTICE_SUCCESS || s_network_connected)')) "boot must retain the Wi-Fi success page after a short-lived connection"
Assert-True (([regex]::Matches($displaySource, 'mode\s*=\s*DISPLAY_MODE_BOOT')).Count -eq 1) "Wi-Fi loss after main-page entry must not re-enter boot mode"
Assert-True ($displaySource.Contains('if (time_ready && flow_animation_due(&snapshot))')) "flow animation must not run from the 00:00 fallback clock"
Assert-True ($displayHeader.Contains('app_display_set_network_connected(bool connected)') -and
             $displaySource.Contains('s_network_connected')) "display must receive the current STA network state"
Assert-True ($displaySource.Contains('s_network_seen') -and
             $displaySource.Contains('if (connected) s_network_seen = true;')) "a successful Wi-Fi link must survive later disconnection during boot"
Assert-True ($displaySource.Contains('boot_ready_for_main') -and
             $displaySource.Contains('time_ready || s_network_seen')) "RTC, SNTP, or an earlier Wi-Fi link must enter the main display"
Assert-True ($displaySource -match '(?s)if \(!boot_ready_for_main\) \{\s*continue;\s*\}') "an unfinished boot lap must not fall through to main-page rendering"
Assert-True (-not $displaySource.Contains('} else if (pending_notice != NETWORK_NOTICE_NONE) {')) "network results after main-page entry must not cover the main page"
Assert-True ($displaySource.Contains('render_startup_frame')) "startup clock sweep renderer is missing"
Assert-True ($displaySource.Contains('DISPLAY_NOTICE_DURATION_MS 1500')) "network result pages must last 1.5 seconds"
Assert-True ($displaySource.Contains('render_network_success')) "network success page renderer is missing"
Assert-True ($displaySource.Contains('render_network_failure')) "network failure page renderer is missing"
Assert-True ($displaySource.Contains('app_display_show_network_success')) "network success display command is missing"
Assert-True ($displaySource.Contains('app_display_show_network_failure')) "network failure display command is missing"
Assert-True ($displaySource.Contains('#define DISPLAY_REFRESH_PERIOD_MS 50')) "display refresh period must be 50ms"
Assert-True ($displaySource.Contains('pdMS_TO_TICKS(DISPLAY_REFRESH_PERIOD_MS)')) "display task must use the configured refresh period"
Assert-True (-not $displaySource.Contains('weather->humidity')) "display must not render humidity"
Assert-True ($displaySource.Contains('#define SECONDS_X 15')) "missing rightmost seconds column"
Assert-True ($displaySource.Contains('(seconds % SECONDS_H) + 1u')) "seconds bar must use a 15-second accumulation cycle"
Assert-True ($displaySource.Contains('SECONDS_Y + SECONDS_H - 1u - row')) "seconds bar must grow from bottom to top"
Assert-True ($displaySource.Contains('ws2812_matrix_clear();') -and $displaySource.Contains('show_frame_with_retry')) "display initialization must send an all-black frame"
Assert-True ($displaySource.Contains('show_frame_with_retry')) "display refresh must use the checked retry helper"
Assert-True ($displaySource.Contains('APP_LOG_ERR')) "display refresh failures must be logged"
Assert-True ($displaySource.Contains('vTaskDelay(pdMS_TO_TICKS(1))')) "display refresh retry must yield before retrying"
Assert-True ($displaySource.Contains('s_refresh_failures')) "display refresh failures must be counted"
Assert-True ($displaySource.Contains('consecutive=%')) "display refresh failure log must include consecutive count"
Assert-True ($displaySource.Contains('s_refresh_failures = 0')) "successful display refresh must clear failure count"
Assert-True ($displaySource.Contains('WS2812 config: gpio=10') -and
             $displaySource.Contains('dma=1') -and
             $displaySource.Contains('pixels=%u')) "WS2812 startup diagnostics are missing"
Assert-True ($displaySource.Contains('recovered after retry')) "WS2812 retry recovery log is missing"
Assert-True ($displaySource.Contains('refresh recovered after %') -or
             $displaySource.Contains('refresh recovered after')) "WS2812 consecutive recovery log is missing"
Assert-True ($displaySource.Contains('tm_min') -and $displaySource.Contains('30')) "missing half-hour animation trigger"
Assert-True ($displaySource.Contains('FLOW_FRAME') -or $displaySource.Contains('flow_animation')) "missing full-screen flow animation"
Assert-True ($weatherSource.Contains('pdMS_TO_TICKS(600000)')) "weather task must refresh every 10 minutes"

$drawio = New-Object System.Xml.XmlDocument
$drawio.Load('D:\Project\WiTime_Hardware\witime_design.drawio')
$diagrams = @($drawio.mxfile.diagram)
Assert-True ($diagrams.Count -eq 3) "draw.io must contain three diagrams"

$ids = @($drawio.SelectNodes('//mxCell') | ForEach-Object { $_.id })
$duplicateIds = @($ids | Group-Object | Where-Object Count -gt 1)
Assert-True ($duplicateIds.Count -eq 0) "draw.io node ids must be unique"
Assert-True (-not $drawio.SelectSingleNode('//mxCell[@id="a4"]')) "flow must not contain RTC validity decision"
Assert-True (-not $drawio.SelectSingleNode('//mxCell[@id="a6"]')) "flow must not contain RTC invalid branch"
$rtcSnapshotNode = $drawio.SelectSingleNode('//mxCell[@id="a5"]')
$rtcSnapshotEdge = $drawio.SelectSingleNode('//mxCell[@id="x03"]')
Assert-True ($rtcSnapshotNode -and $rtcSnapshotNode.value.Contains('from_rtc')) "flow must contain direct RTC snapshot write"
Assert-True ($rtcSnapshotEdge.source -eq 'a3' -and $rtcSnapshotEdge.target -eq 'a5') "RTC flow must be direct"
Assert-True ($drawio.OuterXml.Contains('GPIO8')) "missing GPIO8 mapping"
Assert-True ($drawio.OuterXml.Contains('GPIO10')) "missing GPIO10 mapping"
Assert-True ((Get-Content -Raw main/drivers/ws2812_matrix.c).Contains('serpentine')) "missing serpentine implementation"
Assert-True ($ws2812Source.Contains('.serpentine = false') -and $ws2812Source.Contains('.mirror_x = false') -and $ws2812Source.Contains('.rotation = 1')) "WS2812 mapping must rotate the complete display by 180 degrees"
Assert-True ($ws2812Source.Contains('led_pixel_format = LED_PIXEL_FORMAT_GRB')) "WS2812 driver must use led_strip 2.x pixel format field"
Assert-True (-not $ws2812Source.Contains('color_component_format')) "WS2812 driver uses removed led_strip config field"
Assert-True ($ws2812Source.Contains('led_strip_set_pixel')) "WS2812 driver must use public led_strip_set_pixel API"
Assert-True ($ws2812Source.Contains('led_strip_refresh')) "WS2812 driver must use public led_strip_refresh API"
Assert-True (-not $ws2812Source.Contains('s_strip->set_pixel')) "WS2812 driver must not access opaque led_strip handle"
Assert-True (-not $ws2812Source.Contains('s_strip->refresh')) "WS2812 driver must not access opaque led_strip handle"
Assert-True ($ws2812Source.Contains('#include "driver/gpio.h"')) "WS2812 driver must configure GPIO output quality"
Assert-True ($ws2812Source.Contains('GPIO_DRIVE_CAP_3')) "WS2812 GPIO must use maximum drive strength"
Assert-True ($ws2812Source.Contains('gpio_set_direction(WS2812_GPIO, GPIO_MODE_OUTPUT)') -and
             $ws2812Source.Contains('gpio_set_level(WS2812_GPIO, 0)')) "WS2812 data line must be driven low before RMT initialization"
Assert-True ($ws2812Source.Contains('GPIO_PULLDOWN_ONLY')) "WS2812 data line must retain an internal pull-down between transmissions"
Assert-True ($ws2812Source.Contains('esp_rom_delay_us(300)')) "WS2812 startup low level must exceed the latch interval"
Assert-True ($ws2812Source.Contains('.flags.with_dma = true')) "WS2812 RMT output must use DMA"
Assert-True ($ws2812Source.Contains('s_displayed_frame')) "WS2812 driver must retain the last transmitted frame"
Assert-True ($ws2812Source.Contains('s_displayed_frame_valid')) "WS2812 driver must track whether a frame was transmitted"
Assert-True ($ws2812Source.Contains('memcmp(s_frame, s_displayed_frame, sizeof(s_frame)) == 0')) "unchanged WS2812 frames must be detected"
Assert-True ($ws2812Header.Contains('ws2812_matrix_show_force')) "forced WS2812 refresh API is missing"
Assert-True ($ws2812Header.Contains('ws2812_frame_diagnostics_t') -and
             $ws2812Header.Contains('ws2812_matrix_get_frame_diagnostics')) "WS2812 frame diagnostics API is missing"
Assert-True ($displaySource.Contains('esp_rom_delay_us(WS2812_DUPLICATE_LATCH_US)')) "main frame duplicate refresh must wait for the latch interval"
Assert-True ($displaySource.Contains('ws2812_matrix_show_force(s_brightness_percent)')) "main frames must use forced duplicate refresh"
Assert-True ($displaySource.Contains('diag.grayscale') -and
             $displaySource.Contains('diag.physical_index') -and
             $displaySource.Contains('diag.crc32')) "main frame diagnostics must log color, index and CRC"
Assert-True ($displaySource.Contains('WS2812_DIAGNOSTIC_INTERVAL 60')) "WS2812 diagnostics summary interval must be 60 changed frames"
Assert-True ($displaySource.Contains('WS2812_KEEPALIVE_REFRESH_MS 250')) "validated main frames must be reasserted every 250 ms"
Assert-True ($displaySource.Contains('drop non-grayscale main frame') -and
             $displaySource.Contains('if (!diag.grayscale)')) "non-grayscale main frames must be dropped before transmission"
Assert-True ($displaySource.Contains('s_last_main_force_refresh')) "main frame keepalive timing state is missing"
Assert-True ($ws2812Source.Contains('s_displayed_brightness == brightness_percent')) "brightness changes must still refresh WS2812"
Assert-True ($ws2812Source.Contains('memcpy(s_displayed_frame, s_frame, sizeof(s_frame))')) "only a successful WS2812 refresh may become the displayed frame"
Assert-True ($sdkconfig.Contains('CONFIG_BT_ENABLED=y')) "Bluetooth must be enabled for BluFi"
Assert-True ($sdkconfig.Contains('CONFIG_BT_NIMBLE_ENABLED=y')) "NimBLE must be enabled for BluFi"
Assert-True ($sdkconfig.Contains('CONFIG_BT_NIMBLE_BLUFI_ENABLE=y')) "NimBLE BluFi must be enabled"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_DHM_C=y')) "MbedTLS DHM must be enabled for BluFi security"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_MBEDTLS_DHM_C=y')) "default config must enable MbedTLS DHM"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_DHM_C=y')) "MbedTLS DHM must remain enabled for BluFi"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_AES_C=y')) "MbedTLS AES must remain enabled for BluFi"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_TLS_CLIENT=y')) "MbedTLS TLS client must remain enabled for HTTPS"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y')) "full certificate bundle must remain enabled"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y') -and
             $sdkconfig.Contains('# CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT is not set')) "MbedTLS TLS server must be disabled"
Assert-True ($sdkconfig.Contains('# CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS is not set')) "client TLS session tickets must be disabled"
Assert-True ($sdkconfig.Contains('# CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS is not set')) "server TLS session tickets must be disabled"
Assert-True ($sdkconfig.Contains('# CONFIG_MBEDTLS_SSL_RENEGOTIATION is not set')) "TLS renegotiation must be disabled"
Assert-True (-not $sdkconfig.Contains('CONFIG_MBEDTLS_PKCS7_C=y')) "unused PKCS7 support must be disabled"
Assert-True (-not $sdkconfig.Contains('CONFIG_MBEDTLS_PEM_WRITE_C=y')) "unused PEM write support must be disabled"
Assert-True (-not $sdkconfig.Contains('CONFIG_MBEDTLS_X509_CRL_PARSE_C=y')) "unused X509 CRL support must be disabled"
Assert-True (-not $sdkconfig.Contains('CONFIG_MBEDTLS_X509_CSR_PARSE_C=y')) "unused X509 CSR support must be disabled"
Assert-True (-not $sdkconfig.Contains('CONFIG_MBEDTLS_GCM_SUPPORT_NON_AES_CIPHER=y')) "non-AES GCM support must be disabled"
Assert-True (-not $sdkconfig.Contains('CONFIG_MBEDTLS_ECDSA_DETERMINISTIC=y')) "unused deterministic ECDSA signing must be disabled"
foreach ($curve in @('SECP192R1','SECP224R1','SECP192K1','SECP224K1','SECP256K1','BP256R1','BP384R1','BP512R1')) {
    Assert-True (-not $sdkconfig.Contains("CONFIG_MBEDTLS_ECP_DP_${curve}_ENABLED=y")) "unused curve $curve must be disabled"
}
Assert-True ($sdkconfig.Contains('CONFIG_LWIP_SNTP_MAX_SERVERS=2')) "SNTP must allow two servers"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_LWIP_SNTP_MAX_SERVERS=2')) "default SNTP config must allow two servers"
Assert-True ($sdkconfig.Contains('# CONFIG_LWIP_SNTP_STARTUP_DELAY is not set')) "SNTP startup delay must be disabled"
Assert-True ($sdkconfigDefaults.Contains('# CONFIG_LWIP_SNTP_STARTUP_DELAY is not set')) "default SNTP startup delay must be disabled"
Assert-True ($rootCmake.Contains('SNTP_RECV_TIMEOUT=3000')) "SNTP server receive timeout must be 3 seconds"
Assert-True ($timeSource.Contains('time.cloudflare.com') -and
             $timeSource.Contains('time.google.com')) "global SNTP servers are missing"
Assert-True (-not $timeSource.Contains('s_use_china_ntp') -and
             -not $timeSource.Contains('is_china_timezone')) "SNTP must not select servers from an NVS location"
Assert-True ($timeSource.Contains('esp_err_t app_time_init(void)')) "time service init must not accept persisted location config"
Assert-True ($timeHeader.Contains('app_time_prepare_timezone')) "time service must expose timezone preparation before RTC conversion"
Assert-True ($mainSource.IndexOf('app_time_prepare_timezone()') -lt $mainSource.IndexOf('app_rtc_init()') -and
             $mainSource.IndexOf('app_rtc_init()') -lt $mainSource.IndexOf('app_time_init()')) "timezone must be prepared before RTC while RTC still initializes before network time services"
Assert-True ($rtcSource.Contains('value.time.tm_isdst = -1;')) "RTC local time conversion must let libc determine daylight-saving state"
Assert-True ($rtcSource.Contains('if (now == (time_t)-1)') -and
             $rtcSource.Contains('if (settimeofday(&tv, NULL) != 0)')) "RTC conversion failures must not publish an invalid system time"
Assert-True ($sdkconfig.Contains('CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096')) "main task stack must retain startup headroom"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096')) "default main task stack must retain startup headroom"
Assert-True ($mainSource.Contains('uxTaskGetStackHighWaterMark(NULL)') -and
             $mainSource.Contains('sizeof(StackType_t)')) "startup must log the main task stack watermark in bytes"
Assert-True ($sdkconfig.Contains('CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y') -and $sdkconfig.Contains('CONFIG_ESPTOOLPY_FLASHSIZE="4MB"')) "Flash must be configured as 4MB"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y')) "default Flash config must be 4MB"
Assert-True ($blufiSecuritySource.Contains('mbedtls_dhm_')) "BluFi security must retain DH handshake implementation"
Assert-True (-not $bleProvisionSource.Contains('BluFi STA password raw bytes')) "BluFi must not log Wi-Fi password bytes"
Assert-True (-not $blufiSecuritySource.Contains('blufi shared_key')) "BluFi must not log the DH shared key"
Assert-True (-not $blufiSecuritySource.Contains('blufi session_psk')) "BluFi must not log the session PSK"
Assert-True (-not $bleProvisionSource.Contains('esp_wifi_disconnect();')) "BluFi must delegate STA disconnect and switching to wifi_manager"
Assert-True ($bleProvisionSource.Contains('s_last_sta_reason != WIFI_REASON_ASSOC_LEAVE')) "BluFi must not report an intentional STA switch as a connection failure"
Assert-True ($wifiSource.Contains('WIFI_STA_STATE_IDLE')) "Wi-Fi STA state machine is missing IDLE"
Assert-True ($wifiSource.Contains('WIFI_STA_STATE_CONNECTING')) "Wi-Fi STA state machine is missing CONNECTING"
Assert-True ($wifiSource.Contains('WIFI_STA_STATE_WAIT_IP')) "Wi-Fi STA state machine is missing WAIT_IP"
Assert-True ($wifiSource.Contains('WIFI_STA_STATE_ONLINE')) "Wi-Fi STA state machine is missing ONLINE"
Assert-True ($wifiSource.Contains('WIFI_STA_STATE_SWITCHING')) "Wi-Fi STA state machine is missing SWITCHING"
Assert-True ($wifiSource.Contains('s_pending_sta_config_valid')) "Wi-Fi pending STA configuration is missing"
Assert-True ($wifiSource.Contains('s_connect_generation')) "Wi-Fi reconnect generation guard is missing"
Assert-True ($wifiSource.Contains('WIFI_EVENT_STA_CONNECTED')) "Wi-Fi state machine must track link connection before DHCP"
Assert-True ($wifiSource.Contains('WIFI_REASON_AUTH_EXPIRE')) "Wi-Fi reason 2 must be decoded"
Assert-True ($wifiSource.Contains('WIFI_REASON_ASSOC_LEAVE')) "Wi-Fi reason 8 must be decoded"
Assert-True ($wifiSource.Contains('s_consecutive_sta_failures')) "Wi-Fi consecutive failure counter is missing"
Assert-True ($wifiSource.Contains('wifi_manager_get_sta_failure_count')) "Wi-Fi failure count query is missing"
Assert-True ($wifiSource.Contains('previous_state != WIFI_STA_STATE_SWITCHING') -and
             $wifiSource.Contains('WIFI_REASON_ASSOC_LEAVE')) "intentional STA switches must not count as failures"
Assert-True ($wifiSource.Contains('s_consecutive_sta_failures = 0')) "successful STA connection must reset failure count"
Assert-True ($wifiSource.Contains('record_sta_connection_failure') -and
             $wifiSource.Contains('start STA connect failed')) "synchronous STA start failures must be counted"
Assert-True ($wifiSource.Contains('#define WIFI_FAST_RETRY_DELAY_MS 1000')) "first Wi-Fi retry must use a 1 second delay"
Assert-True ($wifiSource.Contains('#define WIFI_BACKGROUND_RETRY_DELAY_MS 10000')) "post-failure Wi-Fi retry must use a 10 second delay"
Assert-True ($wifiSource.Contains('#define WIFI_RESCAN_FAILURE_THRESHOLD 2')) "Wi-Fi must scan only after two direct failures"
Assert-True ($wifiSource.Contains('has_last_wifi_ssid')) "Wi-Fi must prefer the last successful SSID"
Assert-True ($wifiSource.Contains('app_config_save_last_wifi_ssid')) "Wi-Fi must persist a newly successful SSID"
Assert-True (-not $wifiSource.Contains('app_config_t requested_cfg')) "saved-profile connect must not copy the full app config onto the caller stack"
Assert-True ($wifiSource.Contains('s_saved_cfg = *cfg;') -and
             $wifiSource.Contains('cfg = &s_saved_cfg;')) "saved-profile connect must use the persistent Wi-Fi config snapshot"
Assert-True ($configSource.Contains('erase_optional_key(handle, "last_ssid")')) "clearing Wi-Fi must erase the last successful SSID"
Assert-True ($mainSource.Contains('failure_count == 3U')) "UI must show failure page when the third connection failure is reached"
Assert-True ($mainSource.Contains('app_display_show_network_success')) "main must show the network success page on STA transition"
Assert-True ($mainSource.Contains('app_display_show_network_failure')) "main must show the network failure page after three failures"
Assert-True ($mainSource.Contains('app_display_init(cfg.has_wifi && cfg.wifi_count > 0)')) "main must tell display whether a saved Wi-Fi profile is expected"
Assert-True ($timeSource.Contains('s_sntp_events') -and
             $timeSource.Contains('s_location_events')) "SNTP and location must use independent event channels"
Assert-True ($timeSource.Contains('sntp_task') -and
             $timeSource.Contains('location_task')) "SNTP and location must run in parallel tasks"
$connectedEdge = [regex]::Match($mainSource, '(?s)if \(connected && !s_last_sta_connected\) \{.*?\}')
Assert-True ($connectedEdge.Success -and
             $connectedEdge.Value.Contains('app_time_notify_network')) "network services must start only on the got-IP edge"
Assert-True (-not $wifiSource.Contains('ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA')) "runtime STA config errors must not abort"
Assert-True (-not $wifiSource.Contains('ESP_ERROR_CHECK(esp_wifi_connect())')) "runtime STA connect errors must not abort"
Assert-True ($mainSource.Contains('const bool connected = wifi_manager_is_sta_connected()') -and
             $mainSource.Contains('if (connected && !s_last_sta_connected)')) "time and weather requests must be gated on STA IP state"
Assert-True ($mainCmake.Contains('        driver')) "main component must depend on legacy driver for driver/i2c.h"
Assert-True ($appLogHeader.Contains('APP_LOG_FILE') -and
             $appLogHeader.Contains('__func__') -and
             $appLogHeader.Contains('APP_LOG_ERR')) "app log wrapper must include source context and error details"
$directLogFiles = @(Get-ChildItem main -Recurse -Include *.c,*.h |
    Where-Object { $_.Name -ne 'app_log.h' } |
    Where-Object { (Get-Content -Raw $_.FullName) -match '\bESP_LOG[EWIDV]\s*\(' })
Assert-True ($directLogFiles.Count -eq 0) "business sources must use app_log.h instead of direct ESP_LOG calls"

$matrixPage = @($drawio.mxfile.diagram | Where-Object { $_.id -eq 'matrix-page' })
Assert-True ($matrixPage.Count -eq 1) "missing matrix page"
$matrixRoot = $matrixPage[0].mxGraphModel.root
$matrixClock = $matrixRoot.SelectSingleNode('mxCell[@id="mclock"]')
$matrixSeconds = $matrixRoot.SelectSingleNode('mxCell[@id="mseconds"]')
$matrixPeriod = $matrixRoot.SelectSingleNode('mxCell[@id="mperiod"]')
Assert-True ($matrixClock.value.Contains('13') -and $matrixClock.value.Contains('x=0..12') -and $matrixClock.value.Contains('y=0..4')) "matrix clock must be 13x5"
Assert-True (-not $matrixPage[0].OuterXml.Contains('x=0..12,y=6..9')) "matrix page must keep y=5..9 off"
Assert-True ($matrixPage[0].OuterXml.Contains('x=0..5,y=10..15')) "matrix page weather area is incorrect"
Assert-True ($matrixPage[0].OuterXml.Contains('x=7..12,y=11..15')) "matrix page temperature area is incorrect"
Assert-True ($matrixPage[0].OuterXml.Contains('x=13..14,y=10..11')) "matrix page degree area is incorrect"
Assert-True ($matrixSeconds.value.Contains('x=15') -and $matrixSeconds.value.Contains('y=0..14') -and $matrixSeconds.value.Contains('y=15')) "matrix seconds column is incorrect"
$zOrderLabel = 'Z ' + ([char]0x578b) + ([char]0x9010) + ([char]0x884c) + ([char]0x540c) + ([char]0x5411)
$serpentineLabel = ([char]0x86c7) + ([char]0x5f62) + ' / ' + ([char]0x5f13) + ([char]0x578b)
Assert-True ($matrixPage[0].OuterXml.Contains($zOrderLabel)) "matrix page must document the physical Z-order mapping"
Assert-True (-not $matrixPage[0].OuterXml.Contains($serpentineLabel)) "matrix page must not document the rejected serpentine mapping"
Assert-True ($matrixPeriod -ne $null) "matrix page must document periodic behavior"
$timezoneLabel = ([char]0x65f6) + ([char]0x533a)
Assert-True (-not $matrixPage[0].OuterXml.Contains($timezoneLabel)) "matrix page must not display timezone"

$pinout = New-Object System.Xml.XmlDocument
$pinout.Load($pinoutPath)
Assert-True ($pinout.OuterXml.Contains('GPIO8')) "missing DS3231 SDA mapping"
Assert-True ($pinout.OuterXml.Contains('GPIO9')) "missing DS3231 SCL mapping"
Assert-True ($pinout.OuterXml.Contains('GPIO10')) "missing WS2812 mapping"

Write-Output 'PASS: WiTime static verification'
