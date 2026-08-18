#include "sensors.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "dht.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ha_discovery.h"

#define DHT_SENSOR_COUNT           3
#define DHT_SAMPLE_INTERVAL_MS     30000U

typedef struct {
    const char *name;
    const char *state_key;
    gpio_num_t gpio;
    bool valid;
    bool unavailable_reported;
    int16_t humidity_tenths;
    int16_t temperature_tenths;
    ha_discovery_state_group_handle_t state_group;
} dht11_sensor_t;

static const char *TAG = "sensors";

static dht11_sensor_t s_dht11_sensors[DHT_SENSOR_COUNT] = {
    { "Living Room", "living-room/dht11", GPIO_NUM_13, false, false, 0, 0,
      HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE },
    { "Balcony", "balcony/dht11", GPIO_NUM_15, false, false, 0, 0,
      HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE },
    { "Bedroom", "bedroom/dht11", GPIO_NUM_16, false, false, 0, 0,
      HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE },
};

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static esp_err_t encode_dht11_state(char *payload, size_t payload_size, void *context)
{
    dht11_sensor_t *sensor = context;
    portENTER_CRITICAL(&s_state_lock);
    bool valid = sensor->valid;
    int16_t temperature_tenths = sensor->temperature_tenths;
    int16_t humidity_tenths = sensor->humidity_tenths;
    portEXIT_CRITICAL(&s_state_lock);

    if (!valid) {
        int written = snprintf(payload, payload_size, "{\"available\":false}");
        return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    int temperature_absolute_tenths = temperature_tenths < 0? -(int)temperature_tenths : temperature_tenths;
    const char *temperature_sign = temperature_tenths < 0 ? "-" : "";
    int humidity_rounded = (humidity_tenths + 5) / 10;
    int written = snprintf(payload, payload_size,
                           "{\"available\":true,\"temperature\":%s%d.%d,\"humidity\":%d}",
                           temperature_sign, temperature_absolute_tenths / 10,
                           temperature_absolute_tenths % 10, humidity_rounded);
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void dht_sampling_task(void *argument)
{
    (void)argument;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        for (size_t i = 0; i < DHT_SENSOR_COUNT; ++i) {
            int16_t humidity_tenths;
            int16_t temperature_tenths;
            esp_err_t err = dht_read_data(DHT_TYPE_DHT11, s_dht11_sensors[i].gpio, &humidity_tenths, &temperature_tenths);
            bool should_publish_unavailable;

            portENTER_CRITICAL(&s_state_lock);
            s_dht11_sensors[i].valid = (err == ESP_OK);
            if (s_dht11_sensors[i].valid) {
                s_dht11_sensors[i].humidity_tenths = humidity_tenths;
                s_dht11_sensors[i].temperature_tenths = temperature_tenths;
                s_dht11_sensors[i].unavailable_reported = false;
                should_publish_unavailable = false;
            } else {
                s_dht11_sensors[i].humidity_tenths = 0;
                s_dht11_sensors[i].temperature_tenths = 0;
                should_publish_unavailable = !s_dht11_sensors[i].unavailable_reported;
                s_dht11_sensors[i].unavailable_reported = true;
            }
            portEXIT_CRITICAL(&s_state_lock);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "%s DHT11 read failed: %s", s_dht11_sensors[i].name, esp_err_to_name(err));
                if (should_publish_unavailable) ha_discovery_publish_state_group(s_dht11_sensors[i].state_group);
            } 
            else ha_discovery_publish_state_group(s_dht11_sensors[i].state_group);
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(DHT_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t sensors_init(void)
{
    if (s_initialized) return ESP_OK;

    for (size_t i = 0; i < DHT_SENSOR_COUNT; ++i) {
        const ha_discovery_state_group_config_t state_group_config = {
            .state_key = s_dht11_sensors[i].state_key,
            .encode_payload = encode_dht11_state,
            .context = &s_dht11_sensors[i],
        };
        const ha_discovery_sensor_config_t temperature_config = {
            .entity_key = i == 0 ? "living-room-temperature"
                        : i == 1 ? "balcony-temperature" : "bedroom-temperature",
            .name = i == 0 ? "Living Room Temperature"
                  : i == 1 ? "Balcony Temperature" : "Bedroom Temperature",
            .device_class = "temperature",
            .unit_of_measurement = "\\u00b0C",
            .entity_category = NULL,
            .value_template = "{{ value_json.temperature if value_json.available else 'unavailable' }}",
            .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
            .include_full_device_info = false,
        };
        const ha_discovery_sensor_config_t humidity_config = {
            .entity_key = i == 0 ? "living-room-humidity"
                        : i == 1 ? "balcony-humidity" : "bedroom-humidity",
            .name = i == 0 ? "Living Room Humidity"
                  : i == 1 ? "Balcony Humidity" : "Bedroom Humidity",
            .device_class = "humidity",
            .unit_of_measurement = "%",
            .entity_category = NULL,
            .value_template = "{{ value_json.humidity if value_json.available else 'unavailable' }}",
            .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
            .include_full_device_info = false,
        };

        esp_err_t err = ha_discovery_register_state_group(&state_group_config, &s_dht11_sensors[i].state_group);
        if (err != ESP_OK) return err;

        ha_discovery_sensor_config_t registered_temperature = temperature_config;
        registered_temperature.state_group = s_dht11_sensors[i].state_group;
        ha_discovery_sensor_handle_t ignored_entity;
        err = ha_discovery_register_sensor(&registered_temperature, &ignored_entity);
        if (err != ESP_OK) return err;

        ha_discovery_sensor_config_t registered_humidity = humidity_config;
        registered_humidity.state_group = s_dht11_sensors[i].state_group;
        err = ha_discovery_register_sensor(&registered_humidity, &ignored_entity);
        if (err != ESP_OK) return err;
    }

    if (xTaskCreate(dht_sampling_task, "dht_sampling", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "DHT11 sensors initialized: Living Room GPIO13, Balcony GPIO15, Bedroom GPIO16");
    return ESP_OK;
}
