#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief network 组件发布自定义事件时使用的事件基。
 *
 * 事件通过 ESP-IDF 默认事件循环发布。当前事件不携带额外数据；
 * 其他组件收到 NETWORK_EVENT_CONNECTED 后，可以调用 network_get_ip()
 * 或 network_get_rssi() 查询最新网络信息。
 */
ESP_EVENT_DECLARE_BASE(NETWORK_EVENT);

/**
 * @brief network 组件对外发布的事件编号。
 */
typedef enum {
    /** STA 已连接接入点，并且已经通过 DHCP 获得有效 IPv4 地址。 */
    NETWORK_EVENT_CONNECTED = 0,

    /** 原本可用的网络连接已经丢失，组件正在后台尝试重连。 */
    NETWORK_EVENT_DISCONNECTED,
} network_event_id_t;

/**
 * @brief 初始化 Wi-Fi STA 网络服务。
 *
 * 本函数负责创建 STA 网络接口、Wi-Fi 驱动、事件处理器、重连定时器，
 * 并将 Kconfig 中的 SSID、密码及认证选项写入 Wi-Fi 驱动。
 *
 * 调用本函数前，应用程序必须已经成功完成：
 * - nvs_flash_init()
 * - esp_netif_init()
 * - esp_event_loop_create_default()
 *
 * 本函数不会启动 Wi-Fi，也不会等待连接；实际启动由 network_start() 完成。
 *
 * @return
 * - ESP_OK：初始化成功，或组件已经初始化。
 * - ESP_ERR_INVALID_ARG：Kconfig 中的 SSID 为空。
 * - ESP_ERR_NO_MEM：无法创建 Event Group 等资源。
 * - 其他错误：对应 ESP-IDF Wi-Fi、事件或定时器 API 的错误码。
 */
esp_err_t network_init(void);

/**
 * @brief 启动 Wi-Fi STA。
 *
 * 启动后连接过程由 ESP-IDF 事件循环异步完成，本函数不会等待获得 IP。
 * 同时会关闭 Wi-Fi 省电模式，以提高 MQTT 和 OTA 的响应稳定性。
 *
 * @return ESP_OK 表示驱动已启动；未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t network_start(void);

/**
 * @brief 等待 STA 获得 IPv4 地址。
 *
 * 该函数只等待 NETWORK_CONNECTED_BIT，不会停止或接管重连状态机。
 * 即使返回超时，组件仍会在后台继续重连。
 *
 * @param timeout_ms 最长等待时间，单位为毫秒；UINT32_MAX 表示永久等待。
 *
 * @return
 * - ESP_OK：已经获得 IPv4 地址。
 * - ESP_ERR_TIMEOUT：指定时间内没有获得 IPv4 地址。
 * - ESP_ERR_INVALID_STATE：组件尚未初始化或尚未启动。
 */
esp_err_t network_wait_connected(uint32_t timeout_ms);

/**
 * @brief 查询当前网络是否可用。
 *
 * 只有 STA 已连接并获得有效 IPv4 地址时才返回 true。
 * 仅与接入点完成无线关联、但尚未获得 IP 时仍返回 false。
 */
bool network_is_connected(void);

/**
 * @brief 获取当前 IPv4 地址字符串。
 *
 * @param buffer 接收点分十进制 IPv4 字符串的缓冲区。
 * @param buffer_size 缓冲区大小；IPv4 推荐至少提供 16 字节。
 *
 * @return ESP_OK 表示读取成功；未联网时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t network_get_ip(char *buffer, size_t buffer_size);

/**
 * @brief 获取当前接入点的 Wi-Fi 信号强度。
 *
 * @param rssi 输出 RSSI，单位为 dBm，通常为负数。
 *
 * @return ESP_OK 表示读取成功；未联网时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t network_get_rssi(int8_t *rssi);

/**
 * @brief 主动请求重新连接。
 *
 * 已联网时会先断开，随后由断线事件处理器安排重连；
 * 未联网时会立即请求一次连接，失败后继续使用指数退避。
 */
esp_err_t network_reconnect(void);

/**
 * @brief 停止 Wi-Fi 服务。
 *
 * 停止重连定时器并清除联网状态，但保留初始化资源，
 * 因此后续可以再次调用 network_start()。
 */
esp_err_t network_stop(void);

#ifdef __cplusplus
}
#endif
