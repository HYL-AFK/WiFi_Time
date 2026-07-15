$ErrorActionPreference = 'Stop'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw "FAIL: $message" }
}

function Matrix-Index([int]$x, [int]$y) {
    Assert-True ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) "matrix coordinate out of range"
    return ((15 - $x) * 16 + $y)
}

function Seconds-LitCount([int]$seconds) {
    Assert-True ($seconds -ge 0 -and $seconds -lt 60) "seconds out of range"
    return (($seconds % 15) + 1)
}

function Ds3231-QuarterDegrees([int]$msb, [int]$lsb) {
    if ($msb -ge 128) { $msb -= 256 }
    return ($msb * 4) + (($lsb -shr 6) -band 0x03)
}

Assert-True ((Matrix-Index 15 0) -eq 0) "upper-right logical pixel must map to DIN LED 0"
Assert-True ((Matrix-Index 15 15) -eq 15) "right edge must remain one physical column"
Assert-True ((Matrix-Index 0 0) -eq 240) "upper-left logical pixel must map to the opposite edge"
Assert-True ((Matrix-Index 0 15) -eq 255) "lower-left logical pixel must map to the final LED"
for ($y = 0; $y -lt 16; ++$y) {
    Assert-True ((Matrix-Index 15 $y) -eq $y) "right edge must map to consecutive LEDs"
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
    'D:\Project\WiTime_Hardware\witime_design.drawio'
)
foreach ($path in $required) { Assert-True (Test-Path $path) "missing $path" }
$pinoutPath = Join-Path 'D:\Project\WiTime_Hardware' (([char]0x5f15) + ([char]0x811a) + ([char]0x56fe) + '.drawio')
Assert-True (Test-Path $pinoutPath) "missing user pinout file"

$rtcSource = Get-Content -Raw main/APP/app_rtc.c
$displaySource = Get-Content -Raw main/APP/app_display.c
$timeSource = Get-Content -Raw main/APP/app_time.c
$weatherSource = Get-Content -Raw main/APP/app_weather.c
$ws2812Source = Get-Content -Raw main/drivers/ws2812_matrix.c
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

Assert-True (-not $rtcSource.Contains('value.valid')) "RTC task must not validate RTC time"
Assert-True (-not $displaySource.Contains('t.valid')) "display task must not gate RTC display on valid"
Assert-True (-not $timeSource.Contains('.valid=true')) "SNTP snapshot must not use removed time valid flag"
Assert-True (-not $timeSnapshotHeader.Contains('bool valid')) "time snapshot must not expose RTC validity"
Assert-True (-not $ds3231Source.Contains('out->valid')) "DS3231 driver must not calculate time validity"
$timeResult = [regex]::Match($ds3231Header, '(?s)typedef struct \{.*?\} ds3231_time_t;').Value
Assert-True ($timeResult.Length -gt 0) "missing DS3231 time result type"
Assert-True (-not $timeResult.Contains('bool valid;')) "DS3231 time result must not expose time validity"
Assert-True ($ds3231Source.Contains('#define DS3231_STATUS_REG 0x0f')) "DS3231 status register must use address 0x0F"
Assert-True ($ds3231Source.Contains('read_regs(DS3231_STATUS_REG, &status_reg, 1)')) "DS3231 status register must be read separately"
Assert-True (-not $ds3231Source.Contains('status_reg = r[7]')) "alarm register 0x07 must not be cached as status"
Assert-True ($ds3231Source.Contains('#define DS3231_I2C_HZ 400000')) "DS3231 I2C bus must run at 400kHz"
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
Assert-True ($timeSource.Contains('strcmp(s_timezone, "Asia/Shanghai")')) "timezone response must recognize Asia/Shanghai"
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
Assert-True ($displaySource.Contains('#define CLOCK_HOUR_TENS_X 0')) "clock hour tens coordinate is incorrect"
Assert-True ($displaySource.Contains('#define CLOCK_HOUR_ONES_X 3')) "clock hour ones coordinate is incorrect"
Assert-True ($displaySource.Contains('#define CLOCK_COLON_X 6')) "clock colon coordinate is incorrect"
Assert-True ($displaySource.Contains('#define CLOCK_MINUTE_TENS_X 7')) "clock minute tens must move left one column"
Assert-True ($displaySource.Contains('#define CLOCK_MINUTE_ONES_X 10')) "clock minute ones must move left one column"
Assert-True ($displaySource.Contains('#define TEMP_TENS_X 7') -and $displaySource.Contains('#define TEMP_ONES_X 10')) "temperature digit coordinates are incorrect"
Assert-True ($displaySource.Contains('#define TEMP_Y 10')) "temperature must move up one row"
Assert-True ($displaySource.Contains('#define NEGATIVE_X 6') -and $displaySource.Contains('#define NEGATIVE_Y 12')) "negative temperature marker must move up one row"
Assert-True ($displaySource.Contains('#define DEGREE_X 13') -and $displaySource.Contains('#define DEGREE_Y 9')) "degree symbol must move up with temperature"
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
Assert-True ($displaySource.Contains('#define MATRIX_BRIGHTNESS 35')) "global matrix brightness must be 35 percent"
Assert-True ($displaySource.Contains('DISPLAY_WHITE, DISPLAY_WHITE, DISPLAY_WHITE')) "time, weather, temperature and seconds must use white LEDs"
Assert-True ($displaySource.Contains('#define DISPLAY_REFRESH_PERIOD_MS 50')) "display refresh period must be 50ms"
Assert-True ($displaySource.Contains('pdMS_TO_TICKS(DISPLAY_REFRESH_PERIOD_MS)')) "display task must use the configured refresh period"
Assert-True (-not $displaySource.Contains('weather->humidity')) "display must not render humidity"
Assert-True ($displaySource.Contains('#define SECONDS_X 15')) "missing rightmost seconds column"
Assert-True ($displaySource.Contains('(seconds % SECONDS_H) + 1u')) "seconds bar must use a 15-second accumulation cycle"
Assert-True ($displaySource.Contains('SECONDS_Y + SECONDS_H - 1u - row')) "seconds bar must grow from bottom to top"
Assert-True ($displaySource.Contains('ws2812_matrix_clear();') -and $displaySource.Contains('ws2812_matrix_show(MATRIX_BRIGHTNESS);')) "display initialization must send an all-black frame"
Assert-True ($displaySource.Contains('show_frame_with_retry')) "display refresh must use the checked retry helper"
Assert-True ($displaySource.Contains('APP_LOG_ERR')) "display refresh failures must be logged"
Assert-True ($displaySource.Contains('vTaskDelay(pdMS_TO_TICKS(1))')) "display refresh retry must yield before retrying"
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
Assert-True ($ws2812Source.Contains('.serpentine = false') -and $ws2812Source.Contains('.mirror_x = false') -and $ws2812Source.Contains('.rotation = 3')) "WS2812 mapping must compensate the physical 90-degree rotation"
Assert-True ($ws2812Source.Contains('led_pixel_format = LED_PIXEL_FORMAT_GRB')) "WS2812 driver must use led_strip 2.x pixel format field"
Assert-True (-not $ws2812Source.Contains('color_component_format')) "WS2812 driver uses removed led_strip config field"
Assert-True ($ws2812Source.Contains('led_strip_set_pixel')) "WS2812 driver must use public led_strip_set_pixel API"
Assert-True ($ws2812Source.Contains('led_strip_refresh')) "WS2812 driver must use public led_strip_refresh API"
Assert-True (-not $ws2812Source.Contains('s_strip->set_pixel')) "WS2812 driver must not access opaque led_strip handle"
Assert-True (-not $ws2812Source.Contains('s_strip->refresh')) "WS2812 driver must not access opaque led_strip handle"
Assert-True ($ws2812Source.Contains('#include "driver/gpio.h"')) "WS2812 driver must configure GPIO output quality"
Assert-True ($ws2812Source.Contains('GPIO_DRIVE_CAP_3')) "WS2812 GPIO must use maximum drive strength"
Assert-True ($ws2812Source.Contains('GPIO_FLOATING')) "WS2812 GPIO must disable its internal pull-up"
Assert-True ($ws2812Source.Contains('.flags.with_dma = true')) "WS2812 RMT output must use DMA"
Assert-True ($ws2812Source.Contains('s_displayed_frame')) "WS2812 driver must retain the last transmitted frame"
Assert-True ($ws2812Source.Contains('s_displayed_frame_valid')) "WS2812 driver must track whether a frame was transmitted"
Assert-True ($ws2812Source.Contains('memcmp(s_frame, s_displayed_frame, sizeof(s_frame)) == 0')) "unchanged WS2812 frames must be detected"
Assert-True ($ws2812Source.Contains('s_displayed_brightness == brightness_percent')) "brightness changes must still refresh WS2812"
Assert-True ($ws2812Source.Contains('memcpy(s_displayed_frame, s_frame, sizeof(s_frame))')) "only a successful WS2812 refresh may become the displayed frame"
Assert-True ($sdkconfig.Contains('CONFIG_BT_ENABLED=y')) "Bluetooth must be enabled for BluFi"
Assert-True ($sdkconfig.Contains('CONFIG_BT_NIMBLE_ENABLED=y')) "NimBLE must be enabled for BluFi"
Assert-True ($sdkconfig.Contains('CONFIG_BT_NIMBLE_BLUFI_ENABLE=y')) "NimBLE BluFi must be enabled"
Assert-True ($sdkconfig.Contains('CONFIG_MBEDTLS_DHM_C=y')) "MbedTLS DHM must be enabled for BluFi security"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_MBEDTLS_DHM_C=y')) "default config must enable MbedTLS DHM"
Assert-True ($sdkconfig.Contains('CONFIG_LWIP_SNTP_MAX_SERVERS=2')) "SNTP must allow two servers"
Assert-True ($sdkconfigDefaults.Contains('CONFIG_LWIP_SNTP_MAX_SERVERS=2')) "default SNTP config must allow two servers"
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
Assert-True (-not $wifiSource.Contains('ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA')) "runtime STA config errors must not abort"
Assert-True (-not $wifiSource.Contains('ESP_ERROR_CHECK(esp_wifi_connect())')) "runtime STA connect errors must not abort"
Assert-True ($mainSource.Contains('if (wifi_manager_is_sta_connected())')) "time and weather requests must be gated on STA IP state"
Assert-True ($mainCmake.Contains('        driver')) "main component must depend on legacy driver for driver/i2c.h"

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
