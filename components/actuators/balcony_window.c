#include "balcony_window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ha_discovery.h"
#include "ha_mqtt.h"

#define BALCONY_WINDOW_GPIO             GPIO_NUM_48
#define BALCONY_WINDOW_COMMAND_TOPIC    "smarthome/esp32-1/switch/balcony-window/set"
#define BALCONY_WINDOW_SERVO_FREQUENCY  50U
#define BALCONY_WINDOW_SERVO_RESOLUTION LEDC_TIMER_14_BIT
#define BALCONY_WINDOW_DUTY_CLOSED      1250U
#define BALCONY_WINDOW_DUTY_OPEN        2048U
#define BALCONY_WINDOW_MOVE_STEPS       20U
#define BALCONY_WINDOW_MOVE_STEP_MS     50U

typedef struct {
    bool is_open;
} balcony_window_command_t;

static const char *TAG = "balcony_window";
static QueueHandle_t s_command_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_is_open;
static uint32_t s_current_duty = BALCONY_WINDOW_DUTY_CLOSED;
static bool s_initialized;
static ha_discovery_state_group_handle_t s_state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE;

static esp_err_t encode_window_state(char *payload, size_t payload_size, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    const bool is_open = s_is_open;
    portEXIT_CRITICAL(&s_state_lock);

    const int written = snprintf(payload, payload_size, "%s", is_open ? "ON" : "OFF");
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void publish_window_state(void)
{
    esp_err_t err = ha_discovery_publish_state_group(s_state_group);
    if (err != ESP_OK) ESP_LOGW(TAG, "Window state publish request failed: %s", esp_err_to_name(err));
}

static bool payload_equals(const char *payload, size_t payload_length, const char *expected)
{
    const size_t expected_length = strlen(expected);
    return payload_length == expected_length && memcmp(payload, expected, expected_length) == 0;
}

static esp_err_t enqueue_command(bool is_open)
{
    if (s_command_queue == NULL) return ESP_ERR_INVALID_STATE;
    const balcony_window_command_t command = {.is_open = is_open};
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t handle_mqtt_command(const char *payload, size_t payload_length, void *context)
{
    (void)context;
    if (payload_equals(payload, payload_length, "ON")) return enqueue_command(true);
    if (payload_equals(payload, payload_length, "OFF")) return enqueue_command(false);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t set_servo_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t move_servo(bool is_open)
{
    portENTER_CRITICAL(&s_state_lock);
    const uint32_t start_duty = s_current_duty;
    portEXIT_CRITICAL(&s_state_lock);
    const uint32_t target_duty = is_open ? BALCONY_WINDOW_DUTY_OPEN : BALCONY_WINDOW_DUTY_CLOSED;

    for (uint32_t step = 1; step <= BALCONY_WINDOW_MOVE_STEPS; ++step) {
        const int32_t duty_delta = (int32_t)target_duty - (int32_t)start_duty;
        const uint32_t duty = (uint32_t)((int32_t)start_duty + duty_delta * (int32_t)step / (int32_t)BALCONY_WINDOW_MOVE_STEPS);
        esp_err_t err = set_servo_duty(duty);
        if (err != ESP_OK) {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(BALCONY_WINDOW_MOVE_STEP_MS));
    }

    return ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

static void balcony_window_task(void *argument)
{
    (void)argument;

    while (true) {
        balcony_window_command_t command;
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) continue;

        esp_err_t err = move_servo(command.is_open);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Window servo move failed: %s", esp_err_to_name(err));
            continue;
        }

        portENTER_CRITICAL(&s_state_lock);
        s_is_open = command.is_open;
        s_current_duty = command.is_open ? BALCONY_WINDOW_DUTY_OPEN : BALCONY_WINDOW_DUTY_CLOSED;
        portEXIT_CRITICAL(&s_state_lock);
        publish_window_state();
    }
}

esp_err_t balcony_window_init(void)
{
    if (s_initialized) return ESP_OK;

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BALCONY_WINDOW_SERVO_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BALCONY_WINDOW_SERVO_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) return err;

    const ledc_channel_config_t channel_config = {
        .gpio_num = BALCONY_WINDOW_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) return err;
    err = ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    if (err != ESP_OK) return err;

    s_command_queue = xQueueCreate(4, sizeof(balcony_window_command_t));
    if (s_command_queue == NULL) return ESP_ERR_NO_MEM;

    const ha_discovery_state_group_config_t state_group_config = {
        .state_key = "switch/balcony-window",
        .encode_payload = encode_window_state,
        .context = NULL,
    };
    err = ha_discovery_register_state_group(&state_group_config, &s_state_group);
    if (err != ESP_OK) return err;

    const ha_discovery_switch_config_t discovery_config = {
        .entity_key = "balcony-window",
        .name = "Window - Balcony",
        .command_key = "switch/balcony-window",
        .state_group = s_state_group,
        .include_full_device_info = false,
    };
    ha_discovery_switch_handle_t ignored_switch;
    err = ha_discovery_register_switch(&discovery_config, &ignored_switch);
    if (err != ESP_OK) return err;

    if (xTaskCreate(balcony_window_task, "balcony_window", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = ha_mqtt_register_command_handler(BALCONY_WINDOW_COMMAND_TOPIC, handle_mqtt_command, NULL);
    if (err != ESP_OK) return err;

    /* 每次启动均由执行任务输出约 1 秒的关闭脉冲，使机械与 HA 初始状态一致。 */
    err = enqueue_command(false);
    if (err != ESP_OK) return err;

    s_initialized = true;
    ESP_LOGI(TAG, "Balcony window initialized: GPIO48, 50 Hz servo, closing on startup");
    return ESP_OK;
}
