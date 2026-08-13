#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ha_mqtt 组件通过默认事件循环发布的事件基。
 *
 * 其他业务组件可以监听此事件基，而不需要直接依赖 ESP-MQTT。
 */
ESP_EVENT_DECLARE_BASE(HA_MQTT_EVENT);

/**
 * @brief ha_mqtt 组件对外发布的事件编号。
 */
typedef enum {
    /** 已完成与 Broker 的 MQTT 会话连接，此时可以安全发布业务消息。 */
    HA_MQTT_EVENT_CONNECTED = 0,

    /** MQTT 会话已经断开；ESP-MQTT 会在后台自动尝试重连。 */
    HA_MQTT_EVENT_DISCONNECTED,

    /**
     * Home Assistant 已发布 online。
     *
     * 传感器和执行器组件收到此事件后，应重新发布自己的 MQTT Discovery
     * 配置和当前状态，以恢复 HA 重启后丢失的运行时信息。
     */
    HA_MQTT_EVENT_HOME_ASSISTANT_ONLINE,
} ha_mqtt_event_id_t;

/**
 * @brief 初始化 Home Assistant MQTT 通信组件。
 *
 * 本函数会创建 ESP-MQTT 客户端，注册 MQTT 回调和 NETWORK_EVENT 监听器，
 * 但只有在 network 组件报告已经获得 IPv4 地址后才真正启动客户端。
 * 可以重复调用；成功初始化后再次调用会直接返回 ESP_OK。
 *
 * 调用前必须已经创建 ESP-IDF 默认事件循环。
 *
 * @return
 * - ESP_OK：初始化成功；
 * - ESP_ERR_INVALID_ARG：Broker URI、用户名或密码为空；
 * - 其他错误：事件注册或 ESP-MQTT 客户端初始化失败。
 */
esp_err_t ha_mqtt_init(void);

/**
 * @brief 查询当前是否已经连接到 MQTT Broker。
 */
bool ha_mqtt_is_connected(void);

/**
 * @brief 向指定主题发布一条文本消息。
 *
 * 此接口供后续传感器和执行器组件复用。它只负责投递消息到 ESP-MQTT，
 * 不会等待 Broker 的 PUBACK，因此返回 ESP_OK 代表消息已经成功进入发送流程。
 *
 * @param topic 非空、以 '\0' 结尾的 MQTT 主题。
 * @param payload 非空、以 '\0' 结尾的文本载荷。
 * @param qos QoS 等级，只允许 0 或 1。
 * @param retain 是否要求 Broker 保留最后一条消息。
 *
 * @return
 * - ESP_OK：消息已交给 ESP-MQTT；
 * - ESP_ERR_INVALID_ARG：参数无效；
 * - ESP_ERR_INVALID_STATE：组件未初始化或 MQTT 尚未连接；
 * - ESP_FAIL：ESP-MQTT 拒绝消息或发送队列分配失败。
 */
esp_err_t ha_mqtt_publish(const char *topic,
                          const char *payload,
                          int qos,
                          bool retain);

#ifdef __cplusplus
}
#endif
