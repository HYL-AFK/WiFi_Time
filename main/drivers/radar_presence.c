#include "radar_presence.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_log.h"

/* GPIO13 读取雷达 OUT；距离数据通过 UART1 的 GPIO1/GPIO2 获取。 */
#define RADAR_PRESENCE_GPIO GPIO_NUM_13
#define RADAR_UART_PORT UART_NUM_1
#define RADAR_UART_TX_GPIO GPIO_NUM_1
#define RADAR_UART_RX_GPIO GPIO_NUM_2
#define RADAR_UART_BAUD_RATE 256000
#define RADAR_ACTIVE_LEVEL 1
#define RADAR_SAMPLE_PERIOD_MS 50
/* 小于等于 0.5m 判定为有人，单位为毫米。 */
#define RADAR_DISTANCE_THRESHOLD_MM 500
#define RADAR_MAX_FRAME_SIZE 128
#define RADAR_SEND_STARTUP_CONFIG 0

static const char *TAG = "radar_presence";
static volatile radar_presence_state_t s_state = RADAR_PRESENCE_NONE;
static volatile uint32_t s_distance_mm;
static radar_presence_callback_t s_callback;
static bool s_initialized;
static TaskHandle_t s_task_handle;
static TickType_t s_last_sample_log_tick;
static volatile uint32_t s_rx_bytes;
static volatile uint32_t s_valid_frames;
static volatile uint32_t s_invalid_frames;
static volatile uint32_t s_header_drops;
static bool s_raw_dumped;

#if RADAR_SEND_STARTUP_CONFIG
static const uint8_t s_enable_config[] = {
    0xfd, 0xfc, 0xfb, 0xfa, 0x04, 0x00, 0xff, 0x00,
    0x01, 0x00, 0x04, 0x03, 0x02, 0x01,
};
static const uint8_t s_set_resolution[] = {
    0xfd, 0xfc, 0xfb, 0xfa, 0x04, 0x00, 0xaa, 0x00,
    0x01, 0x00, 0x04, 0x03, 0x02, 0x01,
};
static const uint8_t s_end_config[] = {
    0xfd, 0xfc, 0xfb, 0xfa, 0x02, 0x00, 0xfe, 0x00,
    0x04, 0x03, 0x02, 0x01,
};
#endif

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool has_data_header(const uint8_t *data)
{
    return data[0] == 0xf4 && data[1] == 0xf3 &&
           data[2] == 0xf2 && data[3] == 0xf1;
}

static bool has_data_tail(const uint8_t *data, size_t total_length)
{
    return total_length >= 4 &&
           data[total_length - 4] == 0xf8 &&
           data[total_length - 3] == 0xf7 &&
           data[total_length - 2] == 0xf6 &&
           data[total_length - 1] == 0xf5;
}

static uint16_t nearest_target_cm(uint8_t target_state,
                                  uint16_t moving_cm,
                                  uint16_t static_cm,
                                  uint16_t detected_cm)
{
    /* 移动目标和静止目标同时存在时，取距离更近的目标。 */
    uint16_t nearest = UINT16_MAX;
    if ((target_state & 0x01U) != 0U && moving_cm > 0U) nearest = moving_cm;
    if ((target_state & 0x02U) != 0U && static_cm > 0U && static_cm < nearest) {
        nearest = static_cm;
    }
    if (nearest == UINT16_MAX && detected_cm > 0U) nearest = detected_cm;
    return nearest == UINT16_MAX ? 0U : nearest;
}

static void publish_measurement(uint8_t target_state, uint16_t distance_cm)
{
    /* LD2410C 上报距离单位为厘米，业务层统一转换成毫米。 */
    const uint32_t distance_mm = (uint32_t)distance_cm * 10U;
    const bool detected = target_state != 0U &&
                          distance_mm > 0U &&
                          distance_mm <= RADAR_DISTANCE_THRESHOLD_MM;
    const bool state_changed = detected != (s_state == RADAR_PRESENCE_DETECTED);

    s_distance_mm = distance_mm;
    s_state = detected ? RADAR_PRESENCE_DETECTED : RADAR_PRESENCE_NONE;

    const TickType_t now = xTaskGetTickCount();
    const bool periodic_log_due =
        (now - s_last_sample_log_tick) >= pdMS_TO_TICKS(1000);
    /* 状态变化立即打印，状态不变时每秒打印一次，避免刷爆串口。 */
    if (state_changed || periodic_log_due) {
        APP_LOGI(TAG,
                 "person=%s distance=%" PRIu32 "mm target_state=0x%02x threshold=500mm",
                 detected ? "YES" : "NO",
                 distance_mm,
                 target_state);
        s_last_sample_log_tick = now;
    }
    if (state_changed) {
        if (s_callback != NULL) s_callback(detected, distance_mm);
    }
}

static void parse_data_frame(const uint8_t *frame, size_t total_length)
{
    /* 标准帧：F4 F3 F2 F1 + 长度 + 数据区 + F8 F7 F6 F5。 */
    const uint16_t payload_length = read_le16(&frame[4]);
    if (payload_length < 13U || total_length < (size_t)payload_length + 10U) {
        s_invalid_frames++;
        return;
    }

    const uint8_t *payload = &frame[6];
    if ((payload[0] != 0x01U && payload[0] != 0x02U) ||
        payload[1] != 0xaa ||
        payload[payload_length - 2U] != 0x55 ||
        payload[payload_length - 1U] != 0x00) {
        s_invalid_frames++;
        return;
    }

    /* 数据区格式：类型、标识、目标状态、移动距离/能量、静止距离/能量、检测距离/能量。 */
    const uint8_t target_state = payload[2];
    const uint16_t moving_cm = read_le16(&payload[3]);
    const uint16_t static_cm = read_le16(&payload[6]);
    const uint16_t detected_cm = read_le16(&payload[9]);
    publish_measurement(target_state,
                        nearest_target_cm(target_state,
                                           moving_cm,
                                           static_cm,
                                           detected_cm));
    s_valid_frames++;
}

static void consume_uart_bytes(const uint8_t *data, size_t length)
{
    /* UART 可能一次返回半帧或多帧，使用缓存按帧头和长度重新组帧。 */
    static uint8_t buffer[RADAR_MAX_FRAME_SIZE];
    static size_t buffered;

    for (size_t i = 0; i < length; ++i) {
        s_rx_bytes++;
        if (buffered >= sizeof(buffer)) buffered = 0;
        buffer[buffered++] = data[i];

        for (;;) {
            if (buffered < 4U) break;
            if (!has_data_header(buffer)) {
                s_header_drops++;
                memmove(buffer, &buffer[1], --buffered);
                continue;
            }
            if (buffered < 6U) break;

            const uint16_t payload_length = read_le16(&buffer[4]);
            const size_t total_length = (size_t)payload_length + 10U;
            if (payload_length < 13U || total_length > sizeof(buffer)) {
                s_invalid_frames++;
                memmove(buffer, &buffer[1], --buffered);
                continue;
            }
            if (buffered < total_length) break;
            if (!has_data_tail(buffer, total_length)) {
                s_invalid_frames++;
                memmove(buffer, &buffer[1], --buffered);
                continue;
            }

            parse_data_frame(buffer, total_length);
            buffered -= total_length;
            if (buffered > 0U) {
                memmove(buffer, &buffer[total_length], buffered);
            }
        }
    }
}

#if RADAR_SEND_STARTUP_CONFIG
static esp_err_t send_config_command(const uint8_t *command, size_t length)
{
    int written = uart_write_bytes(RADAR_UART_PORT, command, length);
    if (written != (int)length) return ESP_FAIL;
    return uart_wait_tx_done(RADAR_UART_PORT, pdMS_TO_TICKS(100));
}
#endif

static void configure_distance_resolution(void)
{
#if !RADAR_SEND_STARTUP_CONFIG
    APP_LOGI(TAG, "startup radar configuration skipped; listening at default 256000 baud");
    return;
#else
    (void)uart_flush_input(RADAR_UART_PORT);
    if (send_config_command(s_enable_config, sizeof(s_enable_config)) != ESP_OK) {
        APP_LOGW(TAG, "failed to enable radar configuration");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (send_config_command(s_set_resolution, sizeof(s_set_resolution)) != ESP_OK) {
        APP_LOGW(TAG, "failed to set radar resolution to 0.2m");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (send_config_command(s_end_config, sizeof(s_end_config)) != ESP_OK) {
        APP_LOGW(TAG, "failed to finish radar configuration");
    } else {
        APP_LOGI(TAG, "radar distance resolution request sent: 0.2m");
    }
#endif
}

static void radar_presence_task(void *arg)
{
    (void)arg;
    uint8_t data[128];
    TickType_t last_summary_tick = xTaskGetTickCount();
    TickType_t last_raw_dump_tick = last_summary_tick;
    uint32_t last_valid_frames = 0;
    configure_distance_resolution();

    for (;;) {
        const int length = uart_read_bytes(RADAR_UART_PORT,
                                           data,
                                           sizeof(data),
                                           pdMS_TO_TICKS(RADAR_SAMPLE_PERIOD_MS));
        if (length > 0) {
            /* 只周期性打印少量原始数据，用于现场排查波特率和接线。 */
            const TickType_t now = xTaskGetTickCount();
            if (!s_raw_dumped ||
                (now - last_raw_dump_tick) >= pdMS_TO_TICKS(3000)) {
                const size_t dump_length = (size_t)length < 64U
                                               ? (size_t)length
                                               : 64U;
                APP_LOG_HEX(TAG,
                            s_raw_dumped ? "radar UART sample" : "first radar UART bytes",
                            data,
                            dump_length);
                s_raw_dumped = true;
                last_raw_dump_tick = now;
            }
            consume_uart_bytes(data, (size_t)length);
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - last_summary_tick) >= pdMS_TO_TICKS(1000)) {
            const uint32_t valid_delta = s_valid_frames - last_valid_frames;
            APP_LOGI(TAG,
                     "summary rx_bytes=%" PRIu32 " valid_frames=%" PRIu32
                     " invalid_frames=%" PRIu32 " header_drops=%" PRIu32
                     " frames_last_sec=%" PRIu32 " out_level=%d",
                     s_rx_bytes,
                     s_valid_frames,
                     s_invalid_frames,
                     s_header_drops,
                     valid_delta,
                     gpio_get_level(RADAR_PRESENCE_GPIO));
            if (valid_delta == 0U) {
                APP_LOGW(TAG,
                         "no valid radar frame: check power, GND, TX/RX direction, "
                         "GPIO1/GPIO2 and 256000 baud");
            }
            last_valid_frames = s_valid_frames;
            last_summary_tick = now;
        }
    }
}

esp_err_t radar_presence_init(void)
{
    if (s_initialized) return ESP_OK;

    const gpio_config_t out_config = {
        .pin_bit_mask = 1ULL << RADAR_PRESENCE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_config);
    if (err != ESP_OK) return err;

    const uart_config_t uart_config = {
        .baud_rate = RADAR_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_driver_install(RADAR_UART_PORT, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = uart_param_config(RADAR_UART_PORT, &uart_config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(RADAR_UART_PORT,
                       RADAR_UART_TX_GPIO,
                       RADAR_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    s_state = RADAR_PRESENCE_NONE;
    s_distance_mm = 0;
    if (xTaskCreate(radar_presence_task,
                    "radar_presence",
                    4096,
                    NULL,
                    3,
                    &s_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    APP_LOGI(TAG,
             "initialized: out_gpio=%d uart=%d tx=%d rx=%d baud=%d threshold_mm=%d",
             RADAR_PRESENCE_GPIO,
             RADAR_UART_PORT,
             RADAR_UART_TX_GPIO,
             RADAR_UART_RX_GPIO,
             RADAR_UART_BAUD_RATE,
             RADAR_DISTANCE_THRESHOLD_MM);
    return ESP_OK;
}

void radar_presence_set_callback(radar_presence_callback_t callback)
{
    s_callback = callback;
    if (s_initialized && s_callback != NULL) {
        s_callback(s_state == RADAR_PRESENCE_DETECTED, s_distance_mm);
    }
}

radar_presence_state_t radar_presence_get_state(void)
{
    return s_state;
}

bool radar_presence_is_detected(void)
{
    return radar_presence_get_state() == RADAR_PRESENCE_DETECTED;
}

uint32_t radar_presence_get_distance_mm(void)
{
    return s_distance_mm;
}

TaskHandle_t radar_presence_get_task_handle(void)
{
    return s_task_handle;
}
