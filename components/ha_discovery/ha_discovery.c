#include "ha_discovery.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ha_mqtt.h"

#define HA_DISCOVERY_MAX_SENSORS        32
#define HA_DISCOVERY_MAX_STATE_GROUPS   32
#define HA_DISCOVERY_TASK_STACK_SIZE    6144
#define HA_DISCOVERY_WORK_QUEUE_LENGTH  (HA_DISCOVERY_MAX_SENSORS + HA_DISCOVERY_MAX_STATE_GROUPS)
#define MQTT_QOS                        1
#define MQTT_DISCOVERY_PREFIX           "homeassistant/sensor"
#define MQTT_STATE_PREFIX               "smarthome/esp32-1"
#define MQTT_AVAILABILITY_TOPIC         MQTT_STATE_PREFIX "/status"

typedef struct {
    bool in_use;
    bool publish_pending;
    ha_discovery_state_group_config_t config;
} registered_state_group_t;

typedef struct {
    bool in_use;
    ha_discovery_sensor_config_t config;
} registered_sensor_t;

typedef enum {
    HA_DISCOVERY_WORK_PUBLISH_SENSOR,
    HA_DISCOVERY_WORK_PUBLISH_STATE_GROUP,
} ha_discovery_work_type_t;

typedef struct {
    ha_discovery_work_type_t type;
    size_t handle;
} ha_discovery_work_item_t;

static const char *TAG = "ha_discovery";
static registered_state_group_t s_state_groups[HA_DISCOVERY_MAX_STATE_GROUPS];
static registered_sensor_t s_sensors[HA_DISCOVERY_MAX_SENSORS];
static QueueHandle_t s_work_queue;
static SemaphoreHandle_t s_work_mutex;
static TaskHandle_t s_worker_task;
static uint32_t s_pending_full_syncs;
static bool s_initialized;
static esp_event_handler_instance_t s_mqtt_event_instance;

static void normalize_key(char *destination, size_t destination_size, const char *key)
{
    size_t index = 0;
    while (key[index] != '\0' && index + 1 < destination_size) {
        destination[index] = key[index] == '-' ? '_' : key[index];
        ++index;
    }
    destination[index] = '\0';
}

static esp_err_t publish_text(const char *topic, const char *payload)
{
    return ha_mqtt_publish(topic, payload, MQTT_QOS, true);
}

static void log_publish_failure(const char *what, esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to publish %s: %s", what, esp_err_to_name(err));
    }
}

static bool is_valid_state_group_handle(ha_discovery_state_group_handle_t handle)
{
    return handle < HA_DISCOVERY_MAX_STATE_GROUPS && s_state_groups[handle].in_use;
}

static esp_err_t build_state_topic(ha_discovery_state_group_handle_t handle,
                                   char *state_topic, size_t state_topic_size)
{
    if (!is_valid_state_group_handle(handle)) return ESP_ERR_NOT_FOUND;

    int written = snprintf(state_topic, state_topic_size, "%s/%s/state", MQTT_STATE_PREFIX,
                           s_state_groups[handle].config.state_key);
    return (written >= 0 && (size_t)written < state_topic_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t build_sensor_topics(const ha_discovery_sensor_config_t *config,
                                     char *discovery_topic, size_t discovery_topic_size,
                                     char *state_topic, size_t state_topic_size,
                                     char *unique_id, size_t unique_id_size)
{
    char normalized_key[64];
    normalize_key(normalized_key, sizeof(normalized_key), config->entity_key);

    int written = snprintf(unique_id, unique_id_size, "esp32_1_%s", normalized_key);
    if (written < 0 || (size_t)written >= unique_id_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(discovery_topic, discovery_topic_size, "%s/%s/config", MQTT_DISCOVERY_PREFIX, unique_id);
    if (written < 0 || (size_t)written >= discovery_topic_size) return ESP_ERR_INVALID_SIZE;

    return build_state_topic(config->state_group, state_topic, state_topic_size);
}

/* Runs only in the ha_discovery worker task. */
static esp_err_t publish_discovery_now(ha_discovery_sensor_handle_t handle)
{
    if (handle >= HA_DISCOVERY_MAX_SENSORS || !s_sensors[handle].in_use) return ESP_ERR_NOT_FOUND;

    const ha_discovery_sensor_config_t *config = &s_sensors[handle].config;
    char discovery_topic[160];
    char state_topic[160];
    char unique_id[96];
    char value_template[256];
    char entity_category[96];
    char device[320];
    char payload[1200];

    esp_err_t err = build_sensor_topics(config, discovery_topic, sizeof(discovery_topic),
                                        state_topic, sizeof(state_topic), unique_id,
                                        sizeof(unique_id));
    if (err != ESP_OK) return err;

    if (config->value_template != NULL) {
        snprintf(value_template, sizeof(value_template), ",\"value_template\":\"%s\"", config->value_template);
    } else value_template[0] = '\0';

    if (config->entity_category != NULL) {
        snprintf(entity_category, sizeof(entity_category), ",\"entity_category\":\"%s\"", config->entity_category);
    } else entity_category[0] = '\0';

    if (config->include_full_device_info) {
        snprintf(device, sizeof(device),
                 "{\"identifiers\":[\"esp32-1\"],\"name\":\"ESP32-1\","
                 "\"manufacturer\":\"Espressif\",\"model\":\"ESP32-S3\","
                 "\"sw_version\":\"ESP-IDF 6.0.2\"}");
    } else snprintf(device, sizeof(device), "{\"identifiers\":[\"esp32-1\"]}");

    int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\",\"device_class\":\"%s\","
        "\"state_class\":\"measurement\",\"unit_of_measurement\":\"%s\","
        "\"state_topic\":\"%s\"%s,\"availability_topic\":\"%s\"%s,\"device\":%s}",
        config->name, unique_id, config->device_class, config->unit_of_measurement, state_topic,
        value_template, MQTT_AVAILABILITY_TOPIC, entity_category, device);
    if (written < 0 || (size_t)written >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;

    err = publish_text(discovery_topic, payload);
    log_publish_failure("sensor discovery", err);
    return err;
}

/* Runs only in the ha_discovery worker task. */
static esp_err_t publish_state_group_now(ha_discovery_state_group_handle_t handle)
{
    if (!is_valid_state_group_handle(handle)) return ESP_ERR_NOT_FOUND;

    char state_topic[160];
    char payload[192];
    esp_err_t err = build_state_topic(handle, state_topic, sizeof(state_topic));
    if (err != ESP_OK) return err;

    const ha_discovery_state_group_config_t *config = &s_state_groups[handle].config;
    err = config->encode_payload(payload, sizeof(payload), config->context);
    if (err == ESP_ERR_INVALID_STATE) snprintf(payload, sizeof(payload), "unavailable");
    else if (err != ESP_OK) {
        ESP_LOGW(TAG, "State encoder for '%s' failed: %s", config->state_key, esp_err_to_name(err));
        return err;
    }

    err = publish_text(state_topic, payload);
    log_publish_failure("state group", err);
    return err;
}

static void clear_state_publish_pending(ha_discovery_state_group_handle_t handle)
{
    if (xSemaphoreTake(s_work_mutex, portMAX_DELAY) != pdTRUE) return;
    if (is_valid_state_group_handle(handle)) s_state_groups[handle].publish_pending = false;
    xSemaphoreGive(s_work_mutex);
}

static bool take_pending_full_sync(void)
{
    bool has_pending_sync = false;
    if (xSemaphoreTake(s_work_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (s_pending_full_syncs > 0) {
        --s_pending_full_syncs;
        has_pending_sync = true;
    }
    xSemaphoreGive(s_work_mutex);
    return has_pending_sync;
}

static void publish_all_registered(void)
{
    if (!ha_mqtt_is_connected()) return;

    for (ha_discovery_sensor_handle_t i = 0; i < HA_DISCOVERY_MAX_SENSORS; ++i) {
        if (s_sensors[i].in_use) publish_discovery_now(i);
    }
    for (ha_discovery_state_group_handle_t i = 0; i < HA_DISCOVERY_MAX_STATE_GROUPS; ++i) {
        if (s_state_groups[i].in_use) publish_state_group_now(i);
    }
}

static void ha_discovery_worker_task(void *argument)
{
    (void)argument;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            if (take_pending_full_sync()) {
                publish_all_registered();
                continue;
            }

            ha_discovery_work_item_t work;
            if (xQueueReceive(s_work_queue, &work, 0) != pdTRUE) break;

            if (work.type == HA_DISCOVERY_WORK_PUBLISH_SENSOR) {
                if (ha_mqtt_is_connected()) publish_discovery_now(work.handle);
            } else if (work.type == HA_DISCOVERY_WORK_PUBLISH_STATE_GROUP) {
                /* Clear before encoding so an update during publishing schedules another send. */
                clear_state_publish_pending(work.handle);
                if (ha_mqtt_is_connected()) publish_state_group_now(work.handle);
            }
        }
    }
}

static esp_err_t enqueue_sensor_discovery(ha_discovery_sensor_handle_t handle)
{
    if (s_work_queue == NULL || s_worker_task == NULL) return ESP_ERR_INVALID_STATE;

    const ha_discovery_work_item_t work = {
        .type = HA_DISCOVERY_WORK_PUBLISH_SENSOR,
        .handle = handle,
    };
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discovery work queue is full");
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

static esp_err_t enqueue_full_sync(void)
{
    if (s_worker_task == NULL || xSemaphoreTake(s_work_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_pending_full_syncs == UINT32_MAX) {
        xSemaphoreGive(s_work_mutex);
        ESP_LOGW(TAG, "Full synchronization request counter overflowed");
        return ESP_ERR_NO_MEM;
    }
    ++s_pending_full_syncs;
    xSemaphoreGive(s_work_mutex);
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    (void)event_data;

    if (event_id == HA_MQTT_EVENT_CONNECTED || event_id == HA_MQTT_EVENT_HOME_ASSISTANT_ONLINE) {
        esp_err_t err = enqueue_full_sync();
        if (err != ESP_OK) ESP_LOGW(TAG, "Failed to queue full synchronization: %s", esp_err_to_name(err));
    }
}

esp_err_t ha_discovery_init(void)
{
    if (s_initialized) return ESP_OK;

    s_work_queue = xQueueCreate(HA_DISCOVERY_WORK_QUEUE_LENGTH, sizeof(ha_discovery_work_item_t));
    s_work_mutex = xSemaphoreCreateMutex();

    // 创建队列/互斥量失败时清理
    if (s_work_queue == NULL || s_work_mutex == NULL) {
        if (s_work_queue != NULL) vQueueDelete(s_work_queue);
        if (s_work_mutex != NULL) vSemaphoreDelete(s_work_mutex);
        s_work_queue = NULL;
        s_work_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 创建任务失败时清理队列/互斥量
    if (xTaskCreate(ha_discovery_worker_task, "ha_discovery", HA_DISCOVERY_TASK_STACK_SIZE,
                    NULL, tskIDLE_PRIORITY + 1, &s_worker_task) != pdPASS) {
        vQueueDelete(s_work_queue);
        vSemaphoreDelete(s_work_mutex);
        s_work_queue = NULL;
        s_work_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(
        HA_MQTT_EVENT, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL, &s_mqtt_event_instance);
    if (err != ESP_OK) return err;

    s_initialized = true;
    return ESP_OK;
}

esp_err_t ha_discovery_register_state_group(const ha_discovery_state_group_config_t *config,
                                            ha_discovery_state_group_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->state_key == NULL
        || config->state_key[0] == '\0' || config->encode_payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (ha_discovery_state_group_handle_t i = 0; i < HA_DISCOVERY_MAX_STATE_GROUPS; ++i) {
        if (!s_state_groups[i].in_use) {
            s_state_groups[i].config = *config;
            s_state_groups[i].in_use = true;
            s_state_groups[i].publish_pending = false;
            *handle = i;
            if (ha_mqtt_is_connected()) return ha_discovery_publish_state_group(i);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t ha_discovery_register_sensor(const ha_discovery_sensor_config_t *config,
                                       ha_discovery_sensor_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->entity_key == NULL
        || config->entity_key[0] == '\0' || config->name == NULL || config->device_class == NULL
        || config->unit_of_measurement == NULL || !is_valid_state_group_handle(config->state_group)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (ha_discovery_sensor_handle_t i = 0; i < HA_DISCOVERY_MAX_SENSORS; ++i) {
        if (!s_sensors[i].in_use) {
            s_sensors[i].config = *config;
            s_sensors[i].in_use = true;
            *handle = i;
            if (ha_mqtt_is_connected()) return enqueue_sensor_discovery(i);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t ha_discovery_publish_state_group(ha_discovery_state_group_handle_t handle)
{
    if (!is_valid_state_group_handle(handle) || s_work_queue == NULL || s_worker_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_work_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;

    if (s_state_groups[handle].publish_pending) {
        xSemaphoreGive(s_work_mutex);
        return ESP_OK;
    }

    const ha_discovery_work_item_t work = {
        .type = HA_DISCOVERY_WORK_PUBLISH_STATE_GROUP,
        .handle = handle,
    };
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        xSemaphoreGive(s_work_mutex);
        ESP_LOGW(TAG, "Discovery work queue is full");
        return ESP_ERR_NO_MEM;
    }

    s_state_groups[handle].publish_pending = true;
    xSemaphoreGive(s_work_mutex);
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}
