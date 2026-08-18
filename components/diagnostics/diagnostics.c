#include "diagnostics.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ha_discovery.h"
#include "network.h"

#define RSSI_SAMPLE_INTERVAL_MS     60000U

static const char *TAG = "diagnostics";

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_rssi_valid;
static int8_t s_rssi;
static TaskHandle_t s_rssi_task;
static esp_event_handler_instance_t s_network_event_instance;
static ha_discovery_state_group_handle_t s_rssi_state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE;

static esp_err_t encode_rssi_state(char *payload, size_t payload_size, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    bool valid = s_rssi_valid;
    int8_t rssi = s_rssi;
    portEXIT_CRITICAL(&s_state_lock);
    if (!valid) return ESP_ERR_INVALID_STATE;

    int written = snprintf(payload, payload_size, "%d", rssi);
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void update_rssi(void)
{
    int8_t rssi;
    esp_err_t err = network_get_rssi(&rssi);
    bool changed;
    bool valid = err == ESP_OK;

    portENTER_CRITICAL(&s_state_lock);
    changed = s_rssi_valid != valid || (valid && s_rssi != rssi);
    s_rssi_valid = valid;
    if (valid) s_rssi = rssi;
    portEXIT_CRITICAL(&s_state_lock);

    if (!valid) ESP_LOGW(TAG, "Wi-Fi RSSI is unavailable: %s", esp_err_to_name(err));
    if (valid || changed) ha_discovery_publish_state_group(s_rssi_state_group);
}

static void rssi_task(void *argument)
{
    (void)argument;
    while (true) {
        update_rssi();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RSSI_SAMPLE_INTERVAL_MS));
    }
}

static void network_event_handler(void *handler_args, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_rssi_task != NULL) xTaskNotifyGive(s_rssi_task);
}

esp_err_t diagnostics_init(void)
{
    if (s_initialized) return ESP_OK;

    const ha_discovery_state_group_config_t rssi_state_group_config = {
        .state_key = "diagnostic/wifi-rssi",
        .encode_payload = encode_rssi_state,
        .context = NULL,
    };
    esp_err_t err = ha_discovery_register_state_group(&rssi_state_group_config, &s_rssi_state_group);
    if (err != ESP_OK) return err;

    const ha_discovery_sensor_config_t rssi_config = {
        .entity_key = "wifi-rssi",
        .name = "Wi-Fi RSSI",
        .device_class = "signal_strength",
        .unit_of_measurement = "dBm",
        .entity_category = "diagnostic",
        .value_template = NULL,
        .state_group = s_rssi_state_group,
        .include_full_device_info = true,
    };
    ha_discovery_sensor_handle_t rssi_entity;
    err = ha_discovery_register_sensor(&rssi_config, &rssi_entity);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(NETWORK_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL, &s_network_event_instance);
    if (err != ESP_OK) return err;

    if (xTaskCreate(rssi_task, "wifi_rssi", 3072, NULL, tskIDLE_PRIORITY + 1, &s_rssi_task) != pdPASS) {
        esp_event_handler_instance_unregister(NETWORK_EVENT, ESP_EVENT_ANY_ID, s_network_event_instance);
        s_network_event_instance = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi RSSI diagnostics initialized");
    return ESP_OK;
}
