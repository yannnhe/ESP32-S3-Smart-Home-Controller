#include "binary.h"

#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ha_discovery.h"

#define KITCHEN_FIRE_GPIO        GPIO_NUM_18
#define SENSOR_DEBOUNCE_MS       50U

static const char *TAG = "binary";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_state_valid;
static bool s_fire_detected;
static TaskHandle_t s_fire_task;
static ha_discovery_state_group_handle_t s_fire_state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE;

static esp_err_t encode_fire_state(char *payload, size_t payload_size, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    const bool valid = s_state_valid;
    const bool detected = s_fire_detected;
    portEXIT_CRITICAL(&s_state_lock);

    if (!valid) return ESP_ERR_INVALID_STATE;

    const int written = snprintf(payload, payload_size, "%s", detected ? "ON" : "OFF");
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void update_fire_state(void)
{
    const bool detected = gpio_get_level(KITCHEN_FIRE_GPIO) == 0;

    portENTER_CRITICAL(&s_state_lock);
    const bool changed = !s_state_valid || s_fire_detected != detected;
    s_state_valid = true;
    s_fire_detected = detected;
    portEXIT_CRITICAL(&s_state_lock);

    if (!changed) return;

    esp_err_t err = ha_discovery_publish_state_group(s_fire_state_group);
    if (err != ESP_OK) ESP_LOGW(TAG, "Kitchen fire sensor state publish request failed: %s", esp_err_to_name(err));
}

static void fire_sensor_task(void *argument)
{
    (void)argument;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(SENSOR_DEBOUNCE_MS));
        update_fire_state();
    }
}

static void IRAM_ATTR fire_gpio_isr(void *argument)
{
    (void)argument;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_fire_task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) portYIELD_FROM_ISR();
}

esp_err_t binary_init(void)
{
    if (s_initialized) return ESP_OK;

    const gpio_config_t fire_gpio_config = {
        .pin_bit_mask = 1ULL << KITCHEN_FIRE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&fire_gpio_config);
    if (err != ESP_OK) return err;

    const ha_discovery_state_group_config_t state_group_config = {
        .state_key = "kitchen/fire-sensor",
        .encode_payload = encode_fire_state,
        .context = NULL,
    };
    err = ha_discovery_register_state_group(&state_group_config, &s_fire_state_group);
    if (err != ESP_OK) return err;

    const ha_discovery_binary_sensor_config_t entity_config = {
        .entity_key = "kitchen-fire-sensor",
        .name = "Kitchen Fire Sensor",
        .device_class = "safety",
        .state_group = s_fire_state_group,
        .include_full_device_info = true,
    };
    ha_discovery_binary_sensor_handle_t ignored_entity;
    err = ha_discovery_register_binary_sensor(&entity_config, &ignored_entity);
    if (err != ESP_OK) return err;

    if (xTaskCreate(fire_sensor_task, "fire_sensor", 3072, NULL, tskIDLE_PRIORITY + 1, &s_fire_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = gpio_isr_handler_add(KITCHEN_FIRE_GPIO, fire_gpio_isr, NULL);
    if (err != ESP_OK) return err;

    update_fire_state();
    s_initialized = true;
    ESP_LOGI(TAG, "Kitchen fire sensor initialized on GPIO18 (active low)");
    return ESP_OK;
}
