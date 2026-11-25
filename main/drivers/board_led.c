#include "board_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "app_fault.h"
#include "app_log.h"
#include "wifi_manager.h"

/* ESP32-S3-Zero 板载 WS2812 接 GPIO21（微雪文档「GPIO21 连接 WS2812 RGB LED」）。 */
#define BOARD_LED_GPIO 21
#define BOARD_LED_TICK_MS 50U
#define BOARD_LED_CYCLE_MS 1000U
#define BOARD_LED_GREEN_MS 300U
/* 周期打一条状态心跳：启动阶段日志会被 USB 重枚举吞掉，靠它确认灯在跑。 */
#define BOARD_LED_HEARTBEAT_MS 30000U

/* 板载灯离眼睛近，亮度压到 1/3 左右，满亮会刺眼。 */
#define BOARD_LED_GREEN_R 0
#define BOARD_LED_GREEN_G 48
#define BOARD_LED_GREEN_B 0
#define BOARD_LED_RED_R 96
#define BOARD_LED_RED_G 0
#define BOARD_LED_RED_B 0

static const char *TAG = "board_led";

static led_strip_handle_t s_strip;
static uint8_t s_cur_r;
static uint8_t s_cur_g;
static uint8_t s_cur_b;
static bool s_cur_valid;

/* 只在颜色真的变了才发一帧，避免每 50ms 一次无效刷新。 */
static void apply_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_cur_valid && s_cur_r == r && s_cur_g == g && s_cur_b == b) return;

    s_cur_r = r;
    s_cur_g = g;
    s_cur_b = b;
    s_cur_valid = true;
    (void)led_strip_set_pixel(s_strip, 0, r, g, b);
    (void)led_strip_refresh(s_strip);
}

/* 正常工作 = Wi-Fi 已连上。
   TODO(临时)：DS3231 还没接到板上，先只看 Wi-Fi 让绿灯节奏能验收；
   RTC 接好后恢复成 “Wi-Fi 已连上 && !app_fault_is_active(APP_FAULT_P101_RTC)”。 */
static bool device_healthy(void)
{
    return wifi_manager_is_sta_connected();
}

static void board_led_task(void *arg)
{
    (void)arg;
    uint32_t phase_ms = 0;
    uint32_t elapsed_ms = 0;

    for (;;) {
        const bool healthy = device_healthy();

        if (healthy) {
            if (phase_ms < BOARD_LED_GREEN_MS) {
                apply_color(BOARD_LED_GREEN_R, BOARD_LED_GREEN_G, BOARD_LED_GREEN_B);
            } else {
                apply_color(0, 0, 0);
            }
        } else {
            apply_color(BOARD_LED_RED_R, BOARD_LED_RED_G, BOARD_LED_RED_B);
            /* 恢复时让绿灯从"亮"开始，否则可能接在闪烁的熄灭半段上，看着像没恢复。 */
            phase_ms = BOARD_LED_CYCLE_MS - BOARD_LED_TICK_MS;
        }

        elapsed_ms += BOARD_LED_TICK_MS;
        if (elapsed_ms >= BOARD_LED_HEARTBEAT_MS) {
            elapsed_ms = 0;
            APP_LOGI(TAG, "status=%s wifi=%d rtc_fault=%d",
                     healthy ? "green-blink" : "red-solid",
                     wifi_manager_is_sta_connected() ? 1 : 0,
                     app_fault_is_active(APP_FAULT_P101_RTC) ? 1 : 0);
        }

        phase_ms = (phase_ms + BOARD_LED_TICK_MS) % BOARD_LED_CYCLE_MS;
        vTaskDelay(pdMS_TO_TICKS(BOARD_LED_TICK_MS));
    }
}

esp_err_t board_led_init(void)
{
    if (s_strip != NULL) return ESP_OK;

    const led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "board led init failed, gpio=%d", BOARD_LED_GPIO);
        s_strip = NULL;
        return err;
    }
    (void)led_strip_clear(s_strip);

    if (xTaskCreate(board_led_task, "board_led", 3072, NULL, 2, NULL) != pdPASS) {
        APP_LOGE(TAG, "create board led task failed");
        return ESP_ERR_NO_MEM;
    }

    APP_LOGI(TAG, "board led ready: gpio=%d, green %ums on / %ums off, fault=red solid",
             BOARD_LED_GPIO, (unsigned)BOARD_LED_GREEN_MS,
             (unsigned)(BOARD_LED_CYCLE_MS - BOARD_LED_GREEN_MS));
    return ESP_OK;
}
