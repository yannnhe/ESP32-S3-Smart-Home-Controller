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

/* 各类注册表的固定容量；当前项目在初始化期一次性注册实体。 */
#define HA_DISCOVERY_MAX_SENSORS        32
#define HA_DISCOVERY_MAX_BINARY_SENSORS 16
#define HA_DISCOVERY_MAX_LIGHTS          16
#define HA_DISCOVERY_MAX_SWITCHES        16
#define HA_DISCOVERY_MAX_BUTTONS         8
#define HA_DISCOVERY_MAX_STATE_GROUPS   32
/* 专用发布任务的 FreeRTOS 栈大小（字节）与工作队列最大长度。 */
#define HA_DISCOVERY_TASK_STACK_SIZE    6144
#define HA_DISCOVERY_WORK_QUEUE_LENGTH  (HA_DISCOVERY_MAX_SENSORS + HA_DISCOVERY_MAX_BINARY_SENSORS + HA_DISCOVERY_MAX_LIGHTS + HA_DISCOVERY_MAX_SWITCHES + HA_DISCOVERY_MAX_BUTTONS + HA_DISCOVERY_MAX_STATE_GROUPS)
/* 所有 Discovery 与状态消息均为 retained QoS 1。 */
#define MQTT_QOS                        1
/* Home Assistant Discovery 根主题，以及本设备的状态/在线主题。 */
#define MQTT_SENSOR_DISCOVERY_PREFIX    "homeassistant/sensor"
#define MQTT_BINARY_SENSOR_DISCOVERY_PREFIX "homeassistant/binary_sensor"
#define MQTT_LIGHT_DISCOVERY_PREFIX     "homeassistant/light"
#define MQTT_SWITCH_DISCOVERY_PREFIX    "homeassistant/switch"
#define MQTT_STATE_PREFIX               "smarthome/esp32-1"
#define MQTT_AVAILABILITY_TOPIC         MQTT_STATE_PREFIX "/status"

/** 已注册状态组：一个状态主题及其只读缓存编码回调。 */
typedef struct {
    bool in_use;          /**< 此槽位是否已注册。 */
    bool publish_pending; /**< 已入队但尚未由发布任务处理，供重复请求合并。 */
    ha_discovery_state_group_config_t config; /**< 调用方提供的状态描述。 */
} registered_state_group_t;

/** 已注册的数值 Sensor Discovery 实体。 */
typedef struct {
    bool in_use; /**< 此槽位是否已注册。 */
    ha_discovery_sensor_config_t config; /**< 实体静态配置。 */
} registered_sensor_t;

/** 已注册的 Binary Sensor Discovery 实体。 */
typedef struct {
    bool in_use; /**< 此槽位是否已注册。 */
    ha_discovery_binary_sensor_config_t config; /**< 实体静态配置。 */
} registered_binary_sensor_t;

/** 已注册的 MQTT Light Discovery 实体。 */
typedef struct {
    bool in_use; /**< 此槽位是否已注册。 */
    ha_discovery_light_config_t config; /**< 实体静态配置。 */
} registered_light_t;

/** 已注册的 MQTT Switch Discovery 实体。 */
typedef struct {
    bool in_use; /**< 此槽位是否已注册。 */
    ha_discovery_switch_config_t config; /**< 实体静态配置。 */
} registered_switch_t;

/** 已注册的 MQTT Button Discovery 实体。 */
typedef struct {
    bool in_use;
    ha_discovery_button_config_t config;
} registered_button_t;

/** 发布任务可处理的三类异步工作。 */
typedef enum {
    HA_DISCOVERY_WORK_PUBLISH_SENSOR,        /**< 发布数值 Sensor Discovery JSON。 */
    HA_DISCOVERY_WORK_PUBLISH_BINARY_SENSOR, /**< 发布 Binary Sensor Discovery JSON。 */
    HA_DISCOVERY_WORK_PUBLISH_LIGHT,         /**< 发布 MQTT Light Discovery JSON。 */
    HA_DISCOVERY_WORK_PUBLISH_SWITCH,        /**< 发布 MQTT Switch Discovery JSON。 */
    HA_DISCOVERY_WORK_PUBLISH_BUTTON,        /**< 发布 MQTT Button Discovery JSON。 */
    HA_DISCOVERY_WORK_PUBLISH_STATE_GROUP,   /**< 编码并发布某状态组的最新缓存值。 */
} ha_discovery_work_type_t;

/** 队列中的一个工作项；handle 指向相应注册表的槽位。 */
typedef struct {
    ha_discovery_work_type_t type;
    size_t handle;
} ha_discovery_work_item_t;

static const char *TAG = "ha_discovery"; /**< ESP 日志标签。 */
static registered_state_group_t s_state_groups[HA_DISCOVERY_MAX_STATE_GROUPS]; /**< 状态组注册表。 */
static registered_sensor_t s_sensors[HA_DISCOVERY_MAX_SENSORS]; /**< 数值 Sensor 注册表。 */
static registered_binary_sensor_t s_binary_sensors[HA_DISCOVERY_MAX_BINARY_SENSORS]; /**< 二值实体注册表。 */
static registered_light_t s_lights[HA_DISCOVERY_MAX_LIGHTS]; /**< Light 实体注册表。 */
static registered_switch_t s_switches[HA_DISCOVERY_MAX_SWITCHES]; /**< Switch 实体注册表。 */
static registered_button_t s_buttons[HA_DISCOVERY_MAX_BUTTONS]; /**< Button 实体注册表。 */
static QueueHandle_t s_work_queue; /**< 发布任务消费的工作队列。 */
static SemaphoreHandle_t s_work_mutex; /**< 保护队列去重标志与完整同步计数。 */
static TaskHandle_t s_worker_task; /**< 专用 Discovery/MQTT 发布任务句柄。 */
static uint32_t s_pending_full_syncs; /**< 待执行的“全部 Discovery + 全部状态”次数。 */
static bool s_initialized; /**< 初始化完成标志，避免重复创建资源。 */
static esp_event_handler_instance_t s_mqtt_event_instance; /**< MQTT/HA 事件监听实例。 */

ESP_EVENT_DEFINE_BASE(HA_DISCOVERY_EVENT);

/** 将 entity_key 中的连字符转换为下划线，生成稳定的 HA unique_id 片段。 */
static void normalize_key(char *destination, size_t destination_size, const char *key)
{
    size_t index = 0;
    while (key[index] != '\0' && index + 1 < destination_size) {
        destination[index] = key[index] == '-' ? '_' : key[index];
        ++index;
    }
    destination[index] = '\0';
}

/** 统一以 retained QoS 1 发布一段文本；只允许发布任务调用。 */
static esp_err_t publish_text(const char *topic, const char *payload)
{
    return ha_mqtt_publish(topic, payload, MQTT_QOS, true);
}

/** 忽略“MQTT 未连接”的预期状态，记录其他发布错误。 */
static void log_publish_failure(const char *what, esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to publish %s: %s", what, esp_err_to_name(err));
    }
}

/** 检查状态组句柄是否在范围内且对应槽位已经注册。 */
static bool is_valid_state_group_handle(ha_discovery_state_group_handle_t handle)
{
    return handle < HA_DISCOVERY_MAX_STATE_GROUPS && s_state_groups[handle].in_use;
}

/** 根据状态组的 state_key 组装其 retained MQTT 状态主题。 */
static esp_err_t build_state_topic(ha_discovery_state_group_handle_t handle,
                                   char *state_topic, size_t state_topic_size)
{
    if (!is_valid_state_group_handle(handle)) return ESP_ERR_NOT_FOUND;

    int written = snprintf(state_topic, state_topic_size, "%s/%s/state", MQTT_STATE_PREFIX,
                           s_state_groups[handle].config.state_key);
    return (written >= 0 && (size_t)written < state_topic_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** 根据状态组的 state_key 组装其独立 retained MQTT 可用性主题。 */
static esp_err_t build_state_availability_topic(ha_discovery_state_group_handle_t handle,
                                                char *availability_topic,
                                                size_t availability_topic_size)
{
    if (!is_valid_state_group_handle(handle)) return ESP_ERR_NOT_FOUND;

    const int written = snprintf(availability_topic, availability_topic_size,
                                 "%s/%s/availability", MQTT_STATE_PREFIX,
                                 s_state_groups[handle].config.state_key);
    return (written >= 0 && (size_t)written < availability_topic_size)
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** 为数值 Sensor 生成 unique_id、Discovery topic 和关联状态 topic。 */
static esp_err_t build_sensor_topics(const ha_discovery_sensor_config_t *config,
                                     char *discovery_topic, size_t discovery_topic_size,
                                     char *state_topic, size_t state_topic_size,
                                     char *unique_id, size_t unique_id_size)
{
    char normalized_key[64];
    normalize_key(normalized_key, sizeof(normalized_key), config->entity_key);

    int written = snprintf(unique_id, unique_id_size, "esp32_1_%s", normalized_key);
    if (written < 0 || (size_t)written >= unique_id_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(discovery_topic, discovery_topic_size, "%s/%s/config", MQTT_SENSOR_DISCOVERY_PREFIX, unique_id);
    if (written < 0 || (size_t)written >= discovery_topic_size) return ESP_ERR_INVALID_SIZE;

    return build_state_topic(config->state_group, state_topic, state_topic_size);
}

/** 为 Binary Sensor 生成 unique_id、Discovery topic 和关联状态 topic。 */
static esp_err_t build_binary_sensor_topics(const ha_discovery_binary_sensor_config_t *config,
                                            char *discovery_topic, size_t discovery_topic_size,
                                            char *state_topic, size_t state_topic_size,
                                            char *unique_id, size_t unique_id_size)
{
    char normalized_key[64];
    normalize_key(normalized_key, sizeof(normalized_key), config->entity_key);

    int written = snprintf(unique_id, unique_id_size, "esp32_1_%s", normalized_key);
    if (written < 0 || (size_t)written >= unique_id_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(discovery_topic, discovery_topic_size, "%s/%s/config", MQTT_BINARY_SENSOR_DISCOVERY_PREFIX, unique_id);
    if (written < 0 || (size_t)written >= discovery_topic_size) return ESP_ERR_INVALID_SIZE;

    return build_state_topic(config->state_group, state_topic, state_topic_size);
}

/** 为 MQTT Light 生成 unique_id、Discovery topic、状态 topic 和命令 topic。 */
static esp_err_t build_light_topics(const ha_discovery_light_config_t *config,
                                    char *discovery_topic, size_t discovery_topic_size,
                                    char *state_topic, size_t state_topic_size,
                                    char *command_topic, size_t command_topic_size,
                                    char *unique_id, size_t unique_id_size)
{
    char normalized_key[64];
    normalize_key(normalized_key, sizeof(normalized_key), config->entity_key);

    int written = snprintf(unique_id, unique_id_size, "esp32_1_%s", normalized_key);
    if (written < 0 || (size_t)written >= unique_id_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(discovery_topic, discovery_topic_size, "%s/%s/config",
                       MQTT_LIGHT_DISCOVERY_PREFIX, unique_id);
    if (written < 0 || (size_t)written >= discovery_topic_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(command_topic, command_topic_size, "%s/%s/set", MQTT_STATE_PREFIX,
                       config->command_key);
    if (written < 0 || (size_t)written >= command_topic_size) return ESP_ERR_INVALID_SIZE;

    return build_state_topic(config->state_group, state_topic, state_topic_size);
}

/** 为 MQTT Switch 生成 unique_id、Discovery topic、状态 topic 和命令 topic。 */
static esp_err_t build_switch_topics(const ha_discovery_switch_config_t *config,
                                     char *discovery_topic, size_t discovery_topic_size,
                                     char *state_topic, size_t state_topic_size,
                                     char *command_topic, size_t command_topic_size,
                                     char *unique_id, size_t unique_id_size)
{
    char normalized_key[64];
    normalize_key(normalized_key, sizeof(normalized_key), config->entity_key);

    int written = snprintf(unique_id, unique_id_size, "esp32_1_%s", normalized_key);
    if (written < 0 || (size_t)written >= unique_id_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(discovery_topic, discovery_topic_size, "%s/%s/config",
                       MQTT_SWITCH_DISCOVERY_PREFIX, unique_id);
    if (written < 0 || (size_t)written >= discovery_topic_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(command_topic, command_topic_size, "%s/%s/set", MQTT_STATE_PREFIX,
                       config->command_key);
    if (written < 0 || (size_t)written >= command_topic_size) return ESP_ERR_INVALID_SIZE;

    return build_state_topic(config->state_group, state_topic, state_topic_size);
}

/** 为 MQTT Button 生成 unique_id、Discovery topic 与命令 topic。 */
static esp_err_t build_button_topics(const ha_discovery_button_config_t *config,
                                     char *discovery_topic, size_t discovery_topic_size,
                                     char *command_topic, size_t command_topic_size,
                                     char *unique_id, size_t unique_id_size)
{
    char normalized_key[64];
    normalize_key(normalized_key, sizeof(normalized_key), config->entity_key);

    int written = snprintf(unique_id, unique_id_size, "esp32_1_%s", normalized_key);
    if (written < 0 || (size_t)written >= unique_id_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(discovery_topic, discovery_topic_size, "homeassistant/button/%s/config", unique_id);
    if (written < 0 || (size_t)written >= discovery_topic_size) return ESP_ERR_INVALID_SIZE;

    written = snprintf(command_topic, command_topic_size, "%s/%s/set", MQTT_STATE_PREFIX,
                       config->command_key);
    return (written >= 0 && (size_t)written < command_topic_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** 仅由发布任务调用：构造并发布一个数值 Sensor 的 Discovery JSON。 */
static esp_err_t publish_sensor_discovery_now(ha_discovery_sensor_handle_t handle)
{
    if (handle >= HA_DISCOVERY_MAX_SENSORS || !s_sensors[handle].in_use) return ESP_ERR_NOT_FOUND;

    const ha_discovery_sensor_config_t *config = &s_sensors[handle].config;
    char discovery_topic[160];
    char state_topic[160];
    char state_availability_topic[160];
    char unique_id[96];
    char value_template[256];
    char entity_category[96];
    char state_class[64];
    char device_class[96];
    char unit_of_measurement[96];
    char device[320];
    char payload[1200];

    esp_err_t err = build_sensor_topics(config, discovery_topic, sizeof(discovery_topic),
                                        state_topic, sizeof(state_topic), unique_id,
                                        sizeof(unique_id));
    if (err != ESP_OK) return err;

    err = build_state_availability_topic(config->state_group, state_availability_topic,
                                         sizeof(state_availability_topic));
    if (err != ESP_OK) return err;

    if (config->value_template != NULL) {
        snprintf(value_template, sizeof(value_template), ",\"value_template\":\"%s\"", config->value_template);
    } else value_template[0] = '\0';

    if (config->entity_category != NULL) {
        snprintf(entity_category, sizeof(entity_category), ",\"entity_category\":\"%s\"", config->entity_category);
    } else entity_category[0] = '\0';

    if (config->device_class != NULL) {
        snprintf(device_class, sizeof(device_class), ",\"device_class\":\"%s\"", config->device_class);
    } else device_class[0] = '\0';

    if (config->unit_of_measurement != NULL) {
        snprintf(unit_of_measurement, sizeof(unit_of_measurement), ",\"unit_of_measurement\":\"%s\"",
                 config->unit_of_measurement);
    } else unit_of_measurement[0] = '\0';

    if (config->state_class == NULL) {
        snprintf(state_class, sizeof(state_class), ",\"state_class\":\"measurement\"");
    } else if (config->state_class[0] != '\0') {
        snprintf(state_class, sizeof(state_class), ",\"state_class\":\"%s\"", config->state_class);
    } else state_class[0] = '\0';

    if (config->include_full_device_info) {
        snprintf(device, sizeof(device),
                 "{\"identifiers\":[\"esp32-1\"],\"name\":\"ESP32-1\","
                 "\"manufacturer\":\"Espressif\",\"model\":\"ESP32-S3\","
                 "\"sw_version\":\"ESP-IDF 6.0.2\"}");
    } else snprintf(device, sizeof(device), "{\"identifiers\":[\"esp32-1\"]}");

    int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\"%s%s%s,"
        "\"state_topic\":\"%s\"%s,\"availability\":[{\"topic\":\"%s\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\"},"
        "{\"topic\":\"%s\",\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\"}],\"availability_mode\":\"all\"%s,\"device\":%s}",
        config->name, unique_id, state_class, device_class, unit_of_measurement, state_topic,
        value_template, MQTT_AVAILABILITY_TOPIC, state_availability_topic, entity_category, device);
    if (written < 0 || (size_t)written >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;

    err = publish_text(discovery_topic, payload);
    log_publish_failure("sensor discovery", err);
    return err;
}

/** 仅由发布任务调用：构造并发布一个 Binary Sensor 的 Discovery JSON。 */
static esp_err_t publish_binary_sensor_discovery_now(ha_discovery_binary_sensor_handle_t handle)
{
    if (handle >= HA_DISCOVERY_MAX_BINARY_SENSORS || !s_binary_sensors[handle].in_use) {
        return ESP_ERR_NOT_FOUND;
    }

    const ha_discovery_binary_sensor_config_t *config = &s_binary_sensors[handle].config;
    char discovery_topic[160];
    char state_topic[160];
    char state_availability_topic[160];
    char unique_id[96];
    char device_class[96];
    char device[320];
    char payload[960];

    esp_err_t err = build_binary_sensor_topics(config, discovery_topic, sizeof(discovery_topic),
                                               state_topic, sizeof(state_topic), unique_id,
                                               sizeof(unique_id));
    if (err != ESP_OK) return err;

    err = build_state_availability_topic(config->state_group, state_availability_topic,
                                         sizeof(state_availability_topic));
    if (err != ESP_OK) return err;

    if (config->device_class != NULL) {
        snprintf(device_class, sizeof(device_class), ",\"device_class\":\"%s\"", config->device_class);
    } else device_class[0] = '\0';

    if (config->include_full_device_info) {
        snprintf(device, sizeof(device),
                 "{\"identifiers\":[\"esp32-1\"],\"name\":\"ESP32-1\","
                 "\"manufacturer\":\"Espressif\",\"model\":\"ESP32-S3\","
                 "\"sw_version\":\"ESP-IDF 6.0.2\"}");
    } else snprintf(device, sizeof(device), "{\"identifiers\":[\"esp32-1\"]}");

    int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\"%s,\"state_topic\":\"%s\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"availability\":[{\"topic\":\"%s\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\"},"
        "{\"topic\":\"%s\",\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\"}],\"availability_mode\":\"all\",\"device\":%s}",
        config->name, unique_id, device_class, state_topic, MQTT_AVAILABILITY_TOPIC,
        state_availability_topic, device);
    if (written < 0 || (size_t)written >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;

    err = publish_text(discovery_topic, payload);
    log_publish_failure("binary sensor discovery", err);
    return err;
}

/** 仅由发布任务调用：构造并发布一个 MQTT Light 的 Discovery JSON。 */
static esp_err_t publish_light_discovery_now(ha_discovery_light_handle_t handle)
{
    if (handle >= HA_DISCOVERY_MAX_LIGHTS || !s_lights[handle].in_use) return ESP_ERR_NOT_FOUND;

    const ha_discovery_light_config_t *config = &s_lights[handle].config;
    char discovery_topic[160];
    char state_topic[160];
    char command_topic[160];
    char unique_id[96];
    char brightness[32];
    char device[320];
    char payload[1024];

    esp_err_t err = build_light_topics(config, discovery_topic, sizeof(discovery_topic),
                                       state_topic, sizeof(state_topic), command_topic,
                                       sizeof(command_topic), unique_id, sizeof(unique_id));
    if (err != ESP_OK) return err;

    if (config->supports_brightness) snprintf(brightness, sizeof(brightness), ",\"brightness\":true");
    else brightness[0] = '\0';

    if (config->include_full_device_info) {
        snprintf(device, sizeof(device),
                 "{\"identifiers\":[\"esp32-1\"],\"name\":\"ESP32-1\","
                 "\"manufacturer\":\"Espressif\",\"model\":\"ESP32-S3\","
                 "\"sw_version\":\"ESP-IDF 6.0.2\"}");
    } else snprintf(device, sizeof(device), "{\"identifiers\":[\"esp32-1\"]}");

    int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\",\"schema\":\"json\","
        "\"command_topic\":\"%s\",\"state_topic\":\"%s\"%s,"
        "\"availability_topic\":\"%s\",\"device\":%s}",
        config->name, unique_id, command_topic, state_topic, brightness,
        MQTT_AVAILABILITY_TOPIC, device);
    if (written < 0 || (size_t)written >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;

    err = publish_text(discovery_topic, payload);
    log_publish_failure("light discovery", err);
    return err;
}

/** 仅由发布任务调用：构造并发布一个 MQTT Switch 的 Discovery JSON。 */
static esp_err_t publish_switch_discovery_now(ha_discovery_switch_handle_t handle)
{
    if (handle >= HA_DISCOVERY_MAX_SWITCHES || !s_switches[handle].in_use) return ESP_ERR_NOT_FOUND;

    const ha_discovery_switch_config_t *config = &s_switches[handle].config;
    char discovery_topic[160];
    char state_topic[160];
    char command_topic[160];
    char unique_id[96];
    char device[320];
    char payload[1024];

    esp_err_t err = build_switch_topics(config, discovery_topic, sizeof(discovery_topic),
                                         state_topic, sizeof(state_topic), command_topic,
                                         sizeof(command_topic), unique_id, sizeof(unique_id));
    if (err != ESP_OK) return err;

    if (config->include_full_device_info) {
        snprintf(device, sizeof(device),
                 "{\"identifiers\":[\"esp32-1\"],\"name\":\"ESP32-1\","
                 "\"manufacturer\":\"Espressif\",\"model\":\"ESP32-S3\","
                 "\"sw_version\":\"ESP-IDF 6.0.2\"}");
    } else snprintf(device, sizeof(device), "{\"identifiers\":[\"esp32-1\"]}");

    int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\",\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"availability_topic\":\"%s\",\"device\":%s}",
        config->name, unique_id, command_topic, state_topic, MQTT_AVAILABILITY_TOPIC, device);
    if (written < 0 || (size_t)written >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;

    err = publish_text(discovery_topic, payload);
    log_publish_failure("switch discovery", err);
    return err;
}

/** 仅由发布任务调用：构造并发布一个 MQTT Button 的 Discovery JSON。 */
static esp_err_t publish_button_discovery_now(ha_discovery_button_handle_t handle)
{
    if (handle >= HA_DISCOVERY_MAX_BUTTONS || !s_buttons[handle].in_use) return ESP_ERR_NOT_FOUND;

    const ha_discovery_button_config_t *config = &s_buttons[handle].config;
    char discovery_topic[160];
    char command_topic[160];
    char availability_topic[160];
    char unique_id[96];
    char device[320];
    char payload[1024];

    esp_err_t err = build_button_topics(config, discovery_topic, sizeof(discovery_topic), command_topic,
                                        sizeof(command_topic), unique_id, sizeof(unique_id));
    if (err != ESP_OK) return err;
    err = build_state_availability_topic(config->availability_state_group, availability_topic,
                                         sizeof(availability_topic));
    if (err != ESP_OK) return err;

    if (config->include_full_device_info) {
        snprintf(device, sizeof(device),
                 "{\"identifiers\":[\"esp32-1\"],\"name\":\"ESP32-1\","
                 "\"manufacturer\":\"Espressif\",\"model\":\"ESP32-S3\","
                 "\"sw_version\":\"ESP-IDF 6.0.2\"}");
    } else snprintf(device, sizeof(device), "{\"identifiers\":[\"esp32-1\"]}");

    const int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\",\"command_topic\":\"%s\","
        "\"payload_press\":\"START\",\"availability\":[{\"topic\":\"%s\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\"},"
        "{\"topic\":\"%s\",\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\"}],\"availability_mode\":\"all\",\"device\":%s}",
        config->name, unique_id, command_topic, MQTT_AVAILABILITY_TOPIC, availability_topic, device);
    if (written < 0 || (size_t)written >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;

    err = publish_text(discovery_topic, payload);
    log_publish_failure("button discovery", err);
    return err;
}

/**
 * 仅由发布任务调用：读取业务组件缓存、编码状态并发布。
 * 编码器返回 ESP_ERR_INVALID_STATE 时，将状态组的独立可用性发布为 offline。
 */
static esp_err_t publish_state_group_now(ha_discovery_state_group_handle_t handle)
{
    if (!is_valid_state_group_handle(handle)) return ESP_ERR_NOT_FOUND;

    char state_topic[160];
    char state_availability_topic[160];
    char payload[192];
    esp_err_t err = build_state_topic(handle, state_topic, sizeof(state_topic));
    if (err != ESP_OK) return err;
    err = build_state_availability_topic(handle, state_availability_topic,
                                         sizeof(state_availability_topic));
    if (err != ESP_OK) return err;

    const ha_discovery_state_group_config_t *config = &s_state_groups[handle].config;
    err = config->encode_payload(payload, sizeof(payload), config->context);
    if (err == ESP_ERR_INVALID_STATE) {
        err = publish_text(state_availability_topic, "offline");
        log_publish_failure("state group availability", err);
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "State encoder for '%s' failed: %s", config->state_key, esp_err_to_name(err));
        return err;
    }

    err = publish_text(state_topic, payload);
    log_publish_failure("state group", err);
    if (err != ESP_OK) return err;

    err = publish_text(state_availability_topic, "online");
    log_publish_failure("state group availability", err);
    return err;
}

/** 在状态发布开始前清除合并标志，使发布期间的新更新可再次入队。 */
static void clear_state_publish_pending(ha_discovery_state_group_handle_t handle)
{
    if (xSemaphoreTake(s_work_mutex, portMAX_DELAY) != pdTRUE) return;
    if (is_valid_state_group_handle(handle)) s_state_groups[handle].publish_pending = false;
    xSemaphoreGive(s_work_mutex);
}

/** 取走一个待处理完整同步请求；由发布任务串行调用。 */
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

/** MQTT 连接或 HA 上线后，按“所有 Discovery、所有最新状态”的顺序完整同步。 */
static void publish_all_registered(void)
{
    if (!ha_mqtt_is_connected()) return;

    for (ha_discovery_sensor_handle_t i = 0; i < HA_DISCOVERY_MAX_SENSORS; ++i) {
        if (s_sensors[i].in_use) publish_sensor_discovery_now(i);
    }
    for (ha_discovery_binary_sensor_handle_t i = 0; i < HA_DISCOVERY_MAX_BINARY_SENSORS; ++i) {
        if (s_binary_sensors[i].in_use) publish_binary_sensor_discovery_now(i);
    }
    for (ha_discovery_light_handle_t i = 0; i < HA_DISCOVERY_MAX_LIGHTS; ++i) {
        if (s_lights[i].in_use) publish_light_discovery_now(i);
    }
    for (ha_discovery_switch_handle_t i = 0; i < HA_DISCOVERY_MAX_SWITCHES; ++i) {
        if (s_switches[i].in_use) publish_switch_discovery_now(i);
    }
    for (ha_discovery_button_handle_t i = 0; i < HA_DISCOVERY_MAX_BUTTONS; ++i) {
        if (s_buttons[i].in_use) publish_button_discovery_now(i);
    }
    for (ha_discovery_state_group_handle_t i = 0; i < HA_DISCOVERY_MAX_STATE_GROUPS; ++i) {
        if (s_state_groups[i].in_use) publish_state_group_now(i);
    }

    const esp_err_t err = esp_event_post(HA_DISCOVERY_EVENT, HA_DISCOVERY_EVENT_FULL_SYNC_COMPLETE,
                                         NULL, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish full synchronization event: %s", esp_err_to_name(err));
    }
}

/**
 * 专用发布任务：等待通知后依次处理完整同步和工作队列。
 * 此任务承担 JSON 构造与 MQTT 调用，避免占用 sys_evt/业务任务栈。
 */
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
                if (ha_mqtt_is_connected()) publish_sensor_discovery_now(work.handle);
            } else if (work.type == HA_DISCOVERY_WORK_PUBLISH_BINARY_SENSOR) {
                if (ha_mqtt_is_connected()) publish_binary_sensor_discovery_now(work.handle);
            } else if (work.type == HA_DISCOVERY_WORK_PUBLISH_LIGHT) {
                if (ha_mqtt_is_connected()) publish_light_discovery_now(work.handle);
            } else if (work.type == HA_DISCOVERY_WORK_PUBLISH_SWITCH) {
                if (ha_mqtt_is_connected()) publish_switch_discovery_now(work.handle);
            } else if (work.type == HA_DISCOVERY_WORK_PUBLISH_BUTTON) {
                if (ha_mqtt_is_connected()) publish_button_discovery_now(work.handle);
            } else if (work.type == HA_DISCOVERY_WORK_PUBLISH_STATE_GROUP) {
                /* Clear before encoding so an update during publishing schedules another send. */
                clear_state_publish_pending(work.handle);
                if (ha_mqtt_is_connected()) publish_state_group_now(work.handle);
            }
        }
    }
}

/** 将 MQTT Light Discovery 发布请求放入队列并唤醒发布任务。 */
static esp_err_t enqueue_light_discovery(ha_discovery_light_handle_t handle)
{
    if (s_work_queue == NULL || s_worker_task == NULL) return ESP_ERR_INVALID_STATE;

    const ha_discovery_work_item_t work = {
        .type = HA_DISCOVERY_WORK_PUBLISH_LIGHT,
        .handle = handle,
    };
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discovery work queue is full");
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

/** 将 MQTT Switch Discovery 发布请求放入队列并唤醒发布任务。 */
static esp_err_t enqueue_switch_discovery(ha_discovery_switch_handle_t handle)
{
    if (s_work_queue == NULL || s_worker_task == NULL) return ESP_ERR_INVALID_STATE;

    const ha_discovery_work_item_t work = {
        .type = HA_DISCOVERY_WORK_PUBLISH_SWITCH,
        .handle = handle,
    };
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discovery work queue is full");
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

/** 将 MQTT Button Discovery 发布请求放入队列并唤醒发布任务。 */
static esp_err_t enqueue_button_discovery(ha_discovery_button_handle_t handle)
{
    if (s_work_queue == NULL || s_worker_task == NULL) return ESP_ERR_INVALID_STATE;

    const ha_discovery_work_item_t work = {
        .type = HA_DISCOVERY_WORK_PUBLISH_BUTTON,
        .handle = handle,
    };
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discovery work queue is full");
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

/** 将 Binary Sensor Discovery 发布请求放入队列并唤醒发布任务。 */
static esp_err_t enqueue_binary_sensor_discovery(ha_discovery_binary_sensor_handle_t handle)
{
    if (s_work_queue == NULL || s_worker_task == NULL) return ESP_ERR_INVALID_STATE;

    const ha_discovery_work_item_t work = {
        .type = HA_DISCOVERY_WORK_PUBLISH_BINARY_SENSOR,
        .handle = handle,
    };
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discovery work queue is full");
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

/** 将数值 Sensor Discovery 发布请求放入队列并唤醒发布任务。 */
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

/** 累加一次完整同步请求并通知发布任务；不在事件回调内发布 MQTT。 */
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

/** MQTT 连接或 Home Assistant 上线事件的轻量回调，只请求完整同步。 */
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

/** 创建发布队列、互斥量、专用任务并订阅 MQTT/HA 高层事件。 */
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

/** 在注册表中分配一个状态组；若 MQTT 已连接则异步发布其当前缓存状态。 */
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

/** 在注册表中分配一个数值 Sensor；若 MQTT 已连接则异步发布其 Discovery JSON。 */
esp_err_t ha_discovery_register_sensor(const ha_discovery_sensor_config_t *config,
                                       ha_discovery_sensor_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->entity_key == NULL
        || config->entity_key[0] == '\0' || config->name == NULL
        || !is_valid_state_group_handle(config->state_group)) {
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

/** 在注册表中分配一个 Binary Sensor；若 MQTT 已连接则异步发布其 Discovery JSON。 */
esp_err_t ha_discovery_register_binary_sensor(const ha_discovery_binary_sensor_config_t *config,
                                              ha_discovery_binary_sensor_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->entity_key == NULL
        || config->entity_key[0] == '\0' || config->name == NULL
        || !is_valid_state_group_handle(config->state_group)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (ha_discovery_binary_sensor_handle_t i = 0; i < HA_DISCOVERY_MAX_BINARY_SENSORS; ++i) {
        if (!s_binary_sensors[i].in_use) {
            s_binary_sensors[i].config = *config;
            s_binary_sensors[i].in_use = true;
            *handle = i;
            if (ha_mqtt_is_connected()) return enqueue_binary_sensor_discovery(i);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

/** 在注册表中分配一个 MQTT Light；若 MQTT 已连接则异步发布其 Discovery JSON。 */
esp_err_t ha_discovery_register_light(const ha_discovery_light_config_t *config,
                                      ha_discovery_light_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->entity_key == NULL
        || config->entity_key[0] == '\0' || config->name == NULL || config->command_key == NULL
        || config->command_key[0] == '\0' || !is_valid_state_group_handle(config->state_group)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (ha_discovery_light_handle_t i = 0; i < HA_DISCOVERY_MAX_LIGHTS; ++i) {
        if (!s_lights[i].in_use) {
            s_lights[i].config = *config;
            s_lights[i].in_use = true;
            *handle = i;
            if (ha_mqtt_is_connected()) return enqueue_light_discovery(i);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

/** 在注册表中分配一个 MQTT Switch；若 MQTT 已连接则异步发布其 Discovery JSON。 */
esp_err_t ha_discovery_register_switch(const ha_discovery_switch_config_t *config,
                                       ha_discovery_switch_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->entity_key == NULL
        || config->entity_key[0] == '\0' || config->name == NULL || config->command_key == NULL
        || config->command_key[0] == '\0' || !is_valid_state_group_handle(config->state_group)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (ha_discovery_switch_handle_t i = 0; i < HA_DISCOVERY_MAX_SWITCHES; ++i) {
        if (!s_switches[i].in_use) {
            s_switches[i].config = *config;
            s_switches[i].in_use = true;
            *handle = i;
            if (ha_mqtt_is_connected()) return enqueue_switch_discovery(i);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t ha_discovery_register_button(const ha_discovery_button_config_t *config,
                                       ha_discovery_button_handle_t *handle)
{
    if (!s_initialized || config == NULL || handle == NULL || config->entity_key == NULL
        || config->entity_key[0] == '\0' || config->name == NULL || config->command_key == NULL
        || config->command_key[0] == '\0'
        || !is_valid_state_group_handle(config->availability_state_group)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (ha_discovery_button_handle_t i = 0; i < HA_DISCOVERY_MAX_BUTTONS; ++i) {
        if (!s_buttons[i].in_use) {
            s_buttons[i].config = *config;
            s_buttons[i].in_use = true;
            *handle = i;
            if (ha_mqtt_is_connected()) return enqueue_button_discovery(i);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

/**
 * 供业务组件调用的非阻塞发布请求。
 * 同一状态组已有待处理请求时直接返回成功，发布任务最终读取最新缓存。
 */
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
