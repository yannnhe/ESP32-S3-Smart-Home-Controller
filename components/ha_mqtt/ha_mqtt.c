#include "ha_mqtt.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "network.h"


// 设备级主题约定；后续业务组件可继续在基础主题下划分各自的功能主题
#define HA_MQTT_BASE_TOPIC                    "smarthome/esp32-1"
#define HA_MQTT_AVAILABILITY_TOPIC            HA_MQTT_BASE_TOPIC "/status"
#define HA_MQTT_HA_STATUS_TOPIC               "homeassistant/status"

#define HA_MQTT_PAYLOAD_ONLINE                "online"
#define HA_MQTT_PAYLOAD_OFFLINE               "offline"

#define HA_MQTT_QOS                            1
#define HA_MQTT_MAX_COMMAND_HANDLERS           16

/* "esp32-1-"、六位 MAC 字符串以及结尾 '\0'。 */
#define HA_MQTT_CLIENT_ID_BUFFER_SIZE         16

static const char *TAG = "ha_mqtt";

/* 定义 ha_mqtt.h 中声明的组件自定义事件基。 */
ESP_EVENT_DEFINE_BASE(HA_MQTT_EVENT);

/* ESP-MQTT 客户端句柄由组件统一持有，后续所有发布均通过该句柄完成。 */
static esp_mqtt_client_handle_t s_mqtt_client;

/* 保存 NETWORK_EVENT 处理器实例，便于初始化失败时准确注销。 */
static esp_event_handler_instance_t s_network_event_instance;

/* MQTT client ID 在初始化时由固定设备前缀和 STA MAC 后六位组成。 */
static char s_client_id[HA_MQTT_CLIENT_ID_BUFFER_SIZE];

/*
 * 生命周期状态：
 * initialized 表示客户端及事件监听器已创建；
 * started 表示 esp_mqtt_client_start() 至少成功调用过一次；
 * connected 表示当前 MQTT 会话已经收到 MQTT_EVENT_CONNECTED。
 */
static bool s_initialized;
static bool s_started;
static bool s_connected;

/** 一个已注册命令主题及其只做快速投递的业务回调。 */
typedef struct {
    bool in_use;
    const char *topic;
    ha_mqtt_command_handler_t handler;
    void *context;
} command_handler_registration_t;

static command_handler_registration_t s_command_handlers[HA_MQTT_MAX_COMMAND_HANDLERS];

/**
 * @brief 比较 MQTT 回调中的非 '\0' 结尾字段与普通字符串。
 */
static bool mqtt_field_equals(const char *field, int field_length, const char *expected)
{
    if (field == NULL || field_length < 0 || expected == NULL) return false;
    size_t expected_length = strlen(expected);
    return expected_length == (size_t)field_length && memcmp(field, expected, expected_length) == 0;
}

/**
 * @brief 通过默认事件循环通知其他组件 MQTT 状态发生变化。
 *
 * esp_event_post() 会复制 event_data，因此调用者可以传递栈上临时变量。
 */
static void publish_component_event(ha_mqtt_event_id_t event_id, const void *event_data, size_t event_data_size)
{
    esp_err_t err = esp_event_post(HA_MQTT_EVENT, event_id, event_data, event_data_size, 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "Failed to publish component event %d: %s", event_id, esp_err_to_name(err));
}

/**
 * @brief 将文本消息交给 ESP-MQTT，并统一处理返回值和日志。
 *
 * esp_mqtt_client_publish() 返回的 msg_id >= 0 表示成功进入发送流程；
 * QoS 1 的最终 PUBACK 会异步产生 MQTT_EVENT_PUBLISHED。
 */
static esp_err_t publish_text(const char *topic, const char *payload, int qos, bool retain)
{
    if (s_mqtt_client == NULL || !s_connected) return ESP_ERR_INVALID_STATE;

    int message_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (message_id < 0) {
        ESP_LOGE(TAG, "Failed to publish topic '%s'", topic);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Publish queued: topic='%s', msg_id=%d", topic, message_id);
    return ESP_OK;
}

// 发布 ESP32 在线状态
// online 使用 retained；若设备异常掉电或链路中断，Broker 会用同一主题发布
static void publish_online(void)
{
    esp_err_t err = publish_text(HA_MQTT_AVAILABILITY_TOPIC, HA_MQTT_PAYLOAD_ONLINE, HA_MQTT_QOS, true);
    if (err != ESP_OK) ESP_LOGW(TAG, "Failed to publish availability: %s", esp_err_to_name(err));
}

/** 在 MQTT 建连后重新订阅所有业务命令主题。 */
static void subscribe_registered_command_topics(esp_mqtt_client_handle_t client)
{
    for (size_t i = 0; i < HA_MQTT_MAX_COMMAND_HANDLERS; ++i) {
        if (!s_command_handlers[i].in_use) continue;

        int message_id = esp_mqtt_client_subscribe(client, s_command_handlers[i].topic, HA_MQTT_QOS);
        if (message_id < 0) ESP_LOGE(TAG, "Failed to subscribe command topic '%s'", s_command_handlers[i].topic);
        else ESP_LOGI(TAG, "Command topic subscribed: '%s' (msg_id=%d)", s_command_handlers[i].topic, message_id);
    }
}

/** 将收到的业务命令分派给精确匹配的已注册处理器。 */
static void dispatch_command(const esp_mqtt_event_handle_t event)
{
    for (size_t i = 0; i < HA_MQTT_MAX_COMMAND_HANDLERS; ++i) {
        const command_handler_registration_t *registration = &s_command_handlers[i];
        if (!registration->in_use || !mqtt_field_equals(event->topic, event->topic_len, registration->topic)) continue;

        esp_err_t err = registration->handler(event->data, (size_t)event->data_len, registration->context);
        if (err != ESP_OK) ESP_LOGW(TAG, "Command for '%s' was rejected: %s", registration->topic, esp_err_to_name(err));
        return;
    }
}

// 处理 Broker 下发的 MQTT 数据。
// ESP-MQTT 可能把大载荷拆成多个 MQTT_EVENT_DATA。若未来接收大 JSON，应在此增加按 current_data_offset/total_data_len 组包的缓冲区
static void handle_mqtt_data(const esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        ESP_LOGW(TAG, "Ignored fragmented MQTT payload (%d/%d bytes)", event->data_len, event->total_data_len);
        return;
    }

    if (mqtt_field_equals(event->topic, event->topic_len, HA_MQTT_HA_STATUS_TOPIC)
        && mqtt_field_equals(event->data, event->data_len, HA_MQTT_PAYLOAD_ONLINE)) {
        ESP_LOGI(TAG, "Home Assistant is online; notifying business components");

        /*
         * 先恢复设备级 availability，再通知传感器和执行器组件重新发布各自的
         * Discovery 与状态。offline 消息无需处理，也不对外发布额外事件。
         */
        publish_online();
        publish_component_event(HA_MQTT_EVENT_HOME_ASSISTANT_ONLINE, NULL, 0);
        return;
    }

    dispatch_command(event);
}

/**
 * @brief ESP-MQTT 事件处理器。
 *
 * 该回调运行在 MQTT 客户端任务中，只执行 HA 状态订阅、短消息发布和事件通知，
 * 不执行阻塞式硬件操作。未来传感器和执行器应监听组件自定义事件，
 * 再由各自组件完成 Discovery 和状态发布。
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "Connected to MQTT broker as '%s'", s_client_id);

        /* Clean Session 会让断线前的订阅失效，因此每次建连都重新订阅。 */
        int ha_status_subscribe_id = esp_mqtt_client_subscribe(event->client, HA_MQTT_HA_STATUS_TOPIC, HA_MQTT_QOS);

        if (ha_status_subscribe_id < 0) ESP_LOGE(TAG, "Failed to subscribe to Home Assistant status topic");
        else ESP_LOGI(TAG, "Home Assistant status topic subscribed (msg_id=%d)", ha_status_subscribe_id);

        subscribe_registered_command_topics(event->client);

        /*
         * MQTT 首次连接或重连后先发布设备在线状态，再通知业务组件。
         * 各业务组件可监听 HA_MQTT_EVENT_CONNECTED，发布自己的 Discovery 和状态。
         */
        publish_online();
        publish_component_event(HA_MQTT_EVENT_CONNECTED, NULL, 0);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected from MQTT broker; automatic reconnect remains enabled");
            publish_component_event(HA_MQTT_EVENT_DISCONNECTED, NULL, 0);
        }
        break;

    case MQTT_EVENT_DATA:
        handle_mqtt_data(event);
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle != NULL) {
            ESP_LOGE(TAG, "MQTT error: type=%d, connect_return_code=%d, socket_errno=%d",
                     event->error_handle->error_type,
                     event->error_handle->connect_return_code,
                     event->error_handle->esp_transport_sock_errno);
        } else ESP_LOGE(TAG, "MQTT connection error");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGD(TAG, "Subscription acknowledged, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "QoS message acknowledged, msg_id=%d", event->msg_id);
        break;

    default:
        ESP_LOGD(TAG, "Unhandled MQTT event id=%" PRIi32, event_id);
        break;
    }
}

// 响应 network 组件的高级连接事件
static void network_event_handler(void *handler_args,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)handler_args;
    (void)event_base;
    (void)event_data;

    if (event_id != NETWORK_EVENT_CONNECTED || s_started || s_mqtt_client == NULL) return;

    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "Network is ready; MQTT client started");
    } else ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
}

/**
 * @brief 生成 esp32-1-<STA MAC 后六位> 格式的唯一 client ID。
 */
static esp_err_t build_client_id(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;

    int written = snprintf(s_client_id,
                           sizeof(s_client_id),
                           "esp32-1-%02x%02x%02x",
                           mac[3], mac[4], mac[5]);
    return (written > 0 && (size_t)written < sizeof(s_client_id))
        ? ESP_OK
        : ESP_ERR_INVALID_SIZE;
}

esp_err_t ha_mqtt_init(void)
{
    if (s_initialized) return ESP_OK;

    /* 空凭据是明确的配置错误；日志只提示配置项，不显示任何凭据内容。 */
    if (CONFIG_HA_MQTT_BROKER_URI[0] == '\0'
        || CONFIG_HA_MQTT_USERNAME[0] == '\0'
        || CONFIG_HA_MQTT_PASSWORD[0] == '\0') {
        ESP_LOGE(TAG,
                 "MQTT configuration is incomplete. Configure broker URI, username "
                 "and password in Home Assistant MQTT Configuration");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = build_client_id();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MQTT client ID: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * MQTT 3.1.1 + Clean Session：disable_clean_session=false 即 clean session=true。
     * 自动重连保持启用；last_will 配置为 retained offline，非正常掉线时由 Broker
     * 代替设备发布。普通局域网 mqtt:// 连接当前不配置 TLS 证书。
     */
    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_HA_MQTT_BROKER_URI,
        .credentials = {
            .username = CONFIG_HA_MQTT_USERNAME,
            .client_id = s_client_id,
            .authentication.password = CONFIG_HA_MQTT_PASSWORD,
        },
        .session = {
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
            .keepalive = CONFIG_HA_MQTT_KEEPALIVE_SECONDS,
            .disable_clean_session = false,
            .last_will = {
                .topic = HA_MQTT_AVAILABILITY_TOPIC,
                .msg = HA_MQTT_PAYLOAD_OFFLINE,
                .msg_len = sizeof(HA_MQTT_PAYLOAD_OFFLINE) - 1,
                .qos = HA_MQTT_QOS,
                .retain = true,
            },
        },
        .network.disable_auto_reconnect = false,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create ESP-MQTT client");
        return ESP_ERR_NO_MEM;
    }

    err = esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return err;
    }

    err = esp_event_handler_instance_register(
        NETWORK_EVENT,
        ESP_EVENT_ANY_ID,
        network_event_handler,
        NULL,
        &s_network_event_instance);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MQTT service initialized for broker %s", CONFIG_HA_MQTT_BROKER_URI);

    /* 兼容在网络已经连通之后才初始化本组件的调用顺序。 */
    if (network_is_connected()) {
        err = esp_mqtt_client_start(s_mqtt_client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
            return err;
        }
        s_started = true;
    }

    return ESP_OK;
}

bool ha_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t ha_mqtt_register_command_handler(const char *topic,
                                            ha_mqtt_command_handler_t handler,
                                            void *context)
{
    if (!s_initialized || topic == NULL || topic[0] == '\0' || handler == NULL) return ESP_ERR_INVALID_ARG;

    // 检查是否已有相同topic的注册
    for (size_t i = 0; i < HA_MQTT_MAX_COMMAND_HANDLERS; ++i) {
        if (s_command_handlers[i].in_use && strcmp(s_command_handlers[i].topic, topic) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (size_t i = 0; i < HA_MQTT_MAX_COMMAND_HANDLERS; ++i) {
        if (!s_command_handlers[i].in_use) {
            s_command_handlers[i] = (command_handler_registration_t) {
                .in_use = true,
                .topic = topic,
                .handler = handler,
                .context = context,
            };

            if (s_connected) {
                int message_id = esp_mqtt_client_subscribe(s_mqtt_client, topic, HA_MQTT_QOS);
                if (message_id < 0) return ESP_FAIL;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t ha_mqtt_publish(const char *topic,
                          const char *payload,
                          int qos,
                          bool retain)
{
    if (topic == NULL || topic[0] == '\0' || payload == NULL || (qos != 0 && qos != 1)) return ESP_ERR_INVALID_ARG;
    return publish_text(topic, payload, qos, retain);
}
