#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态编码回调只能读取已缓存的数据，不能读取硬件或执行阻塞操作。 */
typedef esp_err_t (*ha_discovery_state_payload_callback_t)(char *payload,
                                                            size_t payload_size,
                                                            void *context);

typedef size_t ha_discovery_state_group_handle_t;
#define HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE ((ha_discovery_state_group_handle_t)-1)

/**
 * @brief 多个实体共享的 MQTT 状态主题。
 *
 * state_key 会生成 smarthome/esp32-1/<state_key>/state 与
 * smarthome/esp32-1/<state_key>/availability。编码回调返回
 * ESP_ERR_INVALID_STATE 时，公共层将状态组可用性发布为 offline；其他错误不会覆盖旧状态。
 */
typedef struct {
    const char *state_key;
    ha_discovery_state_payload_callback_t encode_payload;
    void *context;
} ha_discovery_state_group_config_t;

/** 一个 Home Assistant MQTT Sensor 实体的静态描述。 */
typedef struct {
    const char *entity_key;
    const char *name;
    const char *device_class;        /**< 可为 NULL，省略 device_class。 */
    const char *unit_of_measurement; /**< 可为 NULL，省略单位。 */
    const char *entity_category;
    const char *value_template;
    /** NULL 使用 measurement；空字符串省略 state_class；其他值按原样发布。 */
    const char *state_class;
    ha_discovery_state_group_handle_t state_group;
    bool include_full_device_info;
} ha_discovery_sensor_config_t;

typedef size_t ha_discovery_sensor_handle_t;
#define HA_DISCOVERY_INVALID_HANDLE ((ha_discovery_sensor_handle_t)-1)

/** 一个 Home Assistant MQTT Binary Sensor 实体的静态描述。 */
typedef struct {
    const char *entity_key;
    const char *name;
    const char *device_class; /**< 可为 NULL，省略 device_class。 */
    ha_discovery_state_group_handle_t state_group;
    bool include_full_device_info;
} ha_discovery_binary_sensor_config_t;

typedef size_t ha_discovery_binary_sensor_handle_t;
#define HA_DISCOVERY_INVALID_BINARY_SENSOR_HANDLE ((ha_discovery_binary_sensor_handle_t)-1)

/** 一个 Home Assistant MQTT Light 实体的静态描述。 */
typedef struct {
    const char *entity_key;
    const char *name;
    const char *command_key; /**< 生成 smarthome/esp32-1/<command_key>/set。 */
    ha_discovery_state_group_handle_t state_group;
    bool supports_brightness;
    bool include_full_device_info;
} ha_discovery_light_config_t;

typedef size_t ha_discovery_light_handle_t;
#define HA_DISCOVERY_INVALID_LIGHT_HANDLE ((ha_discovery_light_handle_t)-1)

/** 一个引用已注册状态组的 Home Assistant MQTT Switch 实体。 */
typedef struct {
    const char *entity_key;
    const char *name;
    const char *command_key; /**< 生成 smarthome/esp32-1/<command_key>/set。 */
    ha_discovery_state_group_handle_t state_group;
    bool include_full_device_info;
} ha_discovery_switch_config_t;

typedef size_t ha_discovery_switch_handle_t;
#define HA_DISCOVERY_INVALID_SWITCH_HANDLE ((ha_discovery_switch_handle_t)-1)

/** 一个引用已注册状态组的 Home Assistant MQTT Button 实体。 */
typedef struct {
    const char *entity_key;
    const char *name;
    const char *command_key; /**< 生成 smarthome/esp32-1/<command_key>/set。 */
    ha_discovery_state_group_handle_t availability_state_group;
    bool include_full_device_info;
} ha_discovery_button_config_t;

typedef size_t ha_discovery_button_handle_t;
#define HA_DISCOVERY_INVALID_BUTTON_HANDLE ((ha_discovery_button_handle_t)-1)

/** Discovery 完整同步结束事件；仅表示 Discovery 任务已提交全部注册内容。 */
ESP_EVENT_DECLARE_BASE(HA_DISCOVERY_EVENT);
typedef enum {
    HA_DISCOVERY_EVENT_FULL_SYNC_COMPLETE = 0,
} ha_discovery_event_id_t;

/** @brief 初始化 Discovery 注册表、MQTT/HA 事件监听和专用发布任务。 */
esp_err_t ha_discovery_init(void);

/** @brief 注册一个状态主题及其无阻塞 JSON/文本编码回调。 */
esp_err_t ha_discovery_register_state_group(const ha_discovery_state_group_config_t *config,
                                            ha_discovery_state_group_handle_t *handle);

/** @brief 注册一个引用已注册状态组的 MQTT Sensor Discovery 实体。 */
esp_err_t ha_discovery_register_sensor(const ha_discovery_sensor_config_t *config,
                                       ha_discovery_sensor_handle_t *handle);

/** @brief 注册一个引用已注册状态组的 MQTT Binary Sensor Discovery 实体。 */
esp_err_t ha_discovery_register_binary_sensor(const ha_discovery_binary_sensor_config_t *config,
                                              ha_discovery_binary_sensor_handle_t *handle);

/** @brief 注册一个引用已注册状态组的 MQTT Light Discovery 实体。 */
esp_err_t ha_discovery_register_light(const ha_discovery_light_config_t *config,
                                      ha_discovery_light_handle_t *handle);

/** @brief 注册一个引用已注册状态组的 MQTT Switch Discovery 实体。 */
esp_err_t ha_discovery_register_switch(const ha_discovery_switch_config_t *config,
                                       ha_discovery_switch_handle_t *handle);

/** @brief 注册一个 MQTT Button；可用性由 availability_state_group 控制。 */
esp_err_t ha_discovery_register_button(const ha_discovery_button_config_t *config,
                                       ha_discovery_button_handle_t *handle);

/**
 * @brief 请求异步发布一个状态组的 retained 当前状态。
 *
 * JSON 编码和 MQTT 发布在 ha_discovery 任务中执行，不在调用方任务中执行。
 * 对同一状态组的重复请求会合并，发布任务只发送最新缓存值。
 */
esp_err_t ha_discovery_publish_state_group(ha_discovery_state_group_handle_t handle);

#ifdef __cplusplus
}
#endif
