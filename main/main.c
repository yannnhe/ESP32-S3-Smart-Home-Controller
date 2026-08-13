#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "ha_mqtt.h"
#include "network.h"

static const char *TAG = "main";

/**
 * @brief 初始化默认 NVS 分区。
 *
 * ESP-IDF 升级或 NVS 页面耗尽时，nvs_flash_init() 可能返回
 * ESP_ERR_NVS_NO_FREE_PAGES 或 ESP_ERR_NVS_NEW_VERSION_FOUND。
 * 当前工程尚无正式业务数据，因此按照官方示例擦除整个默认 NVS 后重建。
 * 后续 storage 组件保存门窗状态后，应进一步评估整分区擦除策略。
 */
static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires recovery; erasing the NVS partition");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

/**
 * @brief 处理 network 组件发布的高级网络状态事件。
 *
 * main 只负责记录基础网络信息。ha_mqtt、后续 SNTP 和 OTA 组件会分别注册
 * 自己的处理器，而不必修改 network 组件内部代码。
 */
static void network_event_handler(void *handler_arg,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)handler_arg;
    (void)event_base;
    (void)event_data;

    if (event_id == NETWORK_EVENT_CONNECTED) {
        /* 网络可用后读取当前 IPv4 和 RSSI，便于串口验证连接质量。 */
        char ip_address[16];
        int8_t rssi;

        if (network_get_ip(ip_address, sizeof(ip_address)) == ESP_OK && network_get_rssi(&rssi) == ESP_OK) {
            ESP_LOGI(TAG, "Network ready: IPv4=%s, RSSI=%d dBm", ip_address, rssi);
        } else {
            ESP_LOGI(TAG, "Network connected");
        }
    } else if (event_id == NETWORK_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Network unavailable; reconnecting in background");
    }
}

void app_main(void)
{
    /*
     * 系统初始化顺序：
     * 1. NVS：Wi-Fi 驱动及未来 storage 组件的持久化基础；
     * 2. esp_netif：TCP/IP 网络接口抽象层；
     * 3. 默认事件循环：Wi-Fi、IP 以及 network 自定义事件的分发基础；
     * 4. 注册 main 的 network 事件监听器；
     * 5. 初始化 ha_mqtt，使其提前监听 network 连接事件；
     * 6. 初始化并启动 network 组件；
     * 7. 有限时间等待首次联网。
     */
    esp_err_t err = initialize_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(err));
        return;
    }

    /* esp_netif_init() 在整个应用生命周期中只应调用一次。 */
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif initialization failed: %s", esp_err_to_name(err));
        return;
    }

    /* Wi-Fi、IP 和自定义 NETWORK_EVENT 共用此默认事件循环。 */
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Default event loop creation failed: %s", esp_err_to_name(err));
        return;
    }

    /* 必须在启动 network 前注册，避免漏掉第一次 CONNECTED 事件。 */
    err = esp_event_handler_register(
        NETWORK_EVENT,
        ESP_EVENT_ANY_ID,
        network_event_handler,
        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network event registration failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * MQTT 组件必须在 network 启动前完成事件注册，以免漏掉首次
     * NETWORK_EVENT_CONNECTED。若凭据尚未填写，只停用 MQTT，Wi-Fi 仍可继续启动，
     * 方便用户通过串口独立验证 network 组件。
     */
    err = ha_mqtt_init();
    if (err == ESP_ERR_INVALID_ARG
        && (CONFIG_HA_MQTT_BROKER_URI[0] == '\0'
            || CONFIG_HA_MQTT_USERNAME[0] == '\0'
            || CONFIG_HA_MQTT_PASSWORD[0] == '\0')) {
        ESP_LOGW(TAG,
                 "MQTT is disabled because its configuration is incomplete. "
                 "Run idf.py menuconfig and open Home Assistant MQTT Configuration.");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT initialization failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * network_init() 遇到空 SSID 会返回 ESP_ERR_INVALID_ARG。
     * 这里把“尚未通过 menuconfig 配置凭据”作为可恢复配置状态处理，
     * 只记录提示并结束 app_main，不触发崩溃或重启循环。
     */
    err = network_init();
    if (err == ESP_ERR_INVALID_ARG && CONFIG_NETWORK_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG,
                 "Network not started because Wi-Fi credentials are empty. "
                 "Run idf.py menuconfig and open Network Service Configuration.");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }

    /* 非阻塞启动；实际连接由 WIFI_EVENT_STA_START 回调发起。 */
    err = network_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network start failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * 首次最多等待 Kconfig 配置的时间。超时只表示目前还没联网，
     * network 内部的 esp_timer 重连状态机仍会继续运行。
     */
    err = network_wait_connected(CONFIG_NETWORK_WIFI_CONNECT_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG,
                 "Initial Wi-Fi connection timed out; "
                 "reconnection continues in the background");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Waiting for network failed: %s", esp_err_to_name(err));
    }
}
