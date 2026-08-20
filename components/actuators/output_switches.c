#include "output_switches.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ha_discovery.h"
#include "ha_mqtt.h"

#define OUTPUT_SWITCH_COUNT        3
#define OUTPUT_SWITCH_QUEUE_LENGTH 8

typedef struct {
    const char *entity_key;
    const char *name;
    const char *state_key;
    const char *command_topic;
    gpio_num_t gpio;
    int off_level;
    bool is_on;
    ha_discovery_state_group_handle_t state_group;
} output_switch_t;

typedef struct {
    uint8_t index;
    bool is_on;
} output_switch_command_t;

static const char *TAG = "output_switches";
static output_switch_t s_switches[OUTPUT_SWITCH_COUNT] = {
    {
        .entity_key = "bedroom-fan",
        .name = "Fan - Bedroom",
        .state_key = "switch/bedroom-fan",
        .command_topic = "smarthome/esp32-1/switch/bedroom-fan/set",
        .gpio = GPIO_NUM_12,
        .off_level = 0,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .entity_key = "kitchen-fan",
        .name = "Fan - Kitchen",
        .state_key = "switch/kitchen-fan",
        .command_topic = "smarthome/esp32-1/switch/kitchen-fan/set",
        .gpio = GPIO_NUM_21,
        .off_level = 0,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .entity_key = "kitchen-buzzer",
        .name = "Buzzer - Kitchen",
        .state_key = "switch/kitchen-buzzer",
        .command_topic = "smarthome/esp32-1/switch/kitchen-buzzer/set",
        .gpio = GPIO_NUM_17,
        .off_level = 1,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
};
static QueueHandle_t s_command_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static esp_err_t encode_switch_state(char *payload, size_t payload_size, void *context)
{
    output_switch_t *output = context;
    portENTER_CRITICAL(&s_state_lock);
    const bool is_on = output->is_on;
    portEXIT_CRITICAL(&s_state_lock);

    const int written = snprintf(payload, payload_size, "%s", is_on ? "ON" : "OFF");
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void publish_switch_state(output_switch_t *output)
{
    esp_err_t err = ha_discovery_publish_state_group(output->state_group);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s state publish request failed: %s", output->name, esp_err_to_name(err));
}

static esp_err_t enqueue_command(uint8_t index, bool is_on)
{
    if (s_command_queue == NULL || index >= OUTPUT_SWITCH_COUNT) return ESP_ERR_INVALID_STATE;

    const output_switch_command_t command = {
        .index = index,
        .is_on = is_on,
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool payload_equals(const char *payload, size_t payload_length, const char *expected)
{
    const size_t expected_length = strlen(expected);
    return payload_length == expected_length && memcmp(payload, expected, expected_length) == 0;
}

static esp_err_t handle_mqtt_command(const char *payload, size_t payload_length, void *context)
{
    output_switch_t *output = context;
    const ptrdiff_t index = output - s_switches;
    if (index < 0 || index >= OUTPUT_SWITCH_COUNT) return ESP_ERR_INVALID_ARG;

    if (payload_equals(payload, payload_length, "ON")) return enqueue_command((uint8_t)index, true);
    if (payload_equals(payload, payload_length, "OFF")) return enqueue_command((uint8_t)index, false);
    return ESP_ERR_INVALID_ARG;
}

static void output_switch_task(void *argument)
{
    (void)argument;

    while (true) {
        output_switch_command_t command;
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command.index >= OUTPUT_SWITCH_COUNT) continue;

        output_switch_t *output = &s_switches[command.index];
        const int level = command.is_on ? !output->off_level : output->off_level;
        esp_err_t err = gpio_set_level(output->gpio, level);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s GPIO update failed: %s", output->name, esp_err_to_name(err));
            continue;
        }

        portENTER_CRITICAL(&s_state_lock);
        output->is_on = command.is_on;
        portEXIT_CRITICAL(&s_state_lock);
        publish_switch_state(output);
    }
}

esp_err_t output_switches_init(void)
{
    if (s_initialized) return ESP_OK;

    for (size_t i = 0; i < OUTPUT_SWITCH_COUNT; ++i) {
        output_switch_t *output = &s_switches[i];
        esp_err_t err = gpio_set_level(output->gpio, output->off_level);
        if (err != ESP_OK) return err;

        const gpio_config_t switch_gpio_config = {
            .pin_bit_mask = 1ULL << output->gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&switch_gpio_config);
        if (err != ESP_OK) return err;
        err = gpio_set_level(output->gpio, output->off_level);
        if (err != ESP_OK) return err;

        const ha_discovery_state_group_config_t state_group_config = {
            .state_key = output->state_key,
            .encode_payload = encode_switch_state,
            .context = output,
        };
        err = ha_discovery_register_state_group(&state_group_config, &output->state_group);
        if (err != ESP_OK) return err;

        const ha_discovery_switch_config_t discovery_config = {
            .entity_key = output->entity_key,
            .name = output->name,
            .command_key = output->state_key,
            .state_group = output->state_group,
            .include_full_device_info = false,
        };
        ha_discovery_switch_handle_t ignored_switch;
        err = ha_discovery_register_switch(&discovery_config, &ignored_switch);
        if (err != ESP_OK) return err;
    }

    s_command_queue = xQueueCreate(OUTPUT_SWITCH_QUEUE_LENGTH, sizeof(output_switch_command_t));
    if (s_command_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(output_switch_task, "output_switches", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < OUTPUT_SWITCH_COUNT; ++i) {
        esp_err_t err = ha_mqtt_register_command_handler(s_switches[i].command_topic, handle_mqtt_command, &s_switches[i]);
        if (err != ESP_OK) return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Output switches initialized: bedroom fan, kitchen fan, kitchen buzzer");
    return ESP_OK;
}
