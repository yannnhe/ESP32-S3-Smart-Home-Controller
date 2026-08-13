#include "network.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

/* Event Group 中用于表示“已经获得 IPv4 地址”的状态位。 */
#define NETWORK_CONNECTED_BIT BIT0

/* 每一轮重连序列的初始等待时间固定为 1 秒。 */
#define NETWORK_RECONNECT_INITIAL_DELAY_MS 1000U

static const char *TAG = "network";

/*
 * 定义 network.h 中声明的自定义事件基。
 * MQTT、OTA、SNTP 等组件可以订阅此事件基，而不必直接处理底层 Wi-Fi 事件。
 */
ESP_EVENT_DEFINE_BASE(NETWORK_EVENT);

/* 网络状态同步对象。当前只使用 BIT0 表示已经获得 IPv4 地址。 */
static EventGroupHandle_t s_network_event_group;

/* 默认 Wi-Fi STA 对应的 esp_netif 对象，用于查询 DHCP 分配的 IPv4 地址。 */
static esp_netif_t *s_sta_netif;

/* 保存事件处理器实例句柄，初始化失败时可以准确注销已经注册的处理器。 */
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

/* 单次 esp_timer，用于非阻塞地实现 1、2、4、8、16、30 秒退避重连。 */
static esp_timer_handle_t s_reconnect_timer;

/*
 * 组件生命周期标志：
 * - initialized：底层驱动、接口、事件和定时器已经创建。
 * - started：Wi-Fi 驱动已经启动，可以发起连接。
 * - stopping：当前正在主动停止，断线事件不应再次安排重连。
 */
static bool s_initialized;
static bool s_started;
static bool s_stopping;

/* 下一次重连前需要等待的时间；联网成功后恢复为 1 秒。 */
static uint32_t s_reconnect_delay_ms = NETWORK_RECONNECT_INITIAL_DELAY_MS;

/*
 * 将 Kconfig 认证模式选项转换成 ESP-IDF wifi_auth_mode_t。
 * 认证阈值表示允许连接的最低安全等级，默认 WPA2 也能接受 WPA3。
 */
#if CONFIG_NETWORK_WIFI_AUTH_WPA3_PSK
#define NETWORK_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_NETWORK_WIFI_AUTH_WPA2_WPA3_PSK
#define NETWORK_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#else
#define NETWORK_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

/* 将 Kconfig 中的 WPA3 SAE 选项转换成驱动使用的 sae_pwe_h2e 枚举值。 */
#if CONFIG_NETWORK_WIFI_SAE_PWE_HUNT_AND_PECK
#define NETWORK_SAE_PWE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#elif CONFIG_NETWORK_WIFI_SAE_PWE_HASH_TO_ELEMENT
#define NETWORK_SAE_PWE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#else
#define NETWORK_SAE_PWE_MODE WPA3_SAE_PWE_BOTH
#endif

/**
 * @brief 读取 Event Group，判断设备当前是否拥有有效 IPv4 地址。
 *
 * 使用一个内部辅助函数统一所有状态判断，避免不同 API 对“已连接”
 * 采用不同标准。
 */
static bool network_has_ip(void)
{
    return s_network_event_group != NULL
        && (xEventGroupGetBits(s_network_event_group) & NETWORK_CONNECTED_BIT) != 0;
}

/**
 * @brief 在 ESP-IDF 默认事件循环中发布 network 自定义事件。
 *
 * 当前函数可能由默认事件循环自己的回调调用，因此投递等待时间设为 0，
 * 避免事件队列已满时阻塞同一个事件任务而形成死锁。
 */
static void publish_network_event(network_event_id_t event_id)
{
    esp_err_t err = esp_event_post(NETWORK_EVENT, event_id, NULL, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish network event %d: %s", event_id, esp_err_to_name(err));
    }
}

/**
 * @brief 停止尚未触发的一次性重连定时器。
 *
 * 定时器未运行时 esp_timer_stop() 返回 ESP_ERR_INVALID_STATE，
 * 对本组件而言属于正常情况，不需要当作错误处理。
 */
static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer != NULL) {
        esp_err_t err = esp_timer_stop(s_reconnect_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop reconnect timer: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief 根据当前退避时间安排下一次重连。
 *
 * 本函数不直接阻塞或延时，而是启动一次性 esp_timer。每次成功安排后，
 * 下一次等待时间翻倍，并受 CONFIG_NETWORK_WIFI_RECONNECT_MAX_BACKOFF_MS
 * 限制。
 */
static void schedule_reconnect(void)
{
    /* 主动停止期间绝不能因为断线事件再次唤起 Wi-Fi。 */
    if (!s_started || s_stopping || s_reconnect_timer == NULL) return;

    /* 保证任何时刻最多只有一个待触发的重连定时器。 */
    stop_reconnect_timer();

    uint32_t delay_ms = s_reconnect_delay_ms;
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule Wi-Fi reconnect: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Next Wi-Fi reconnect attempt in %" PRIu32 " ms", delay_ms);

    /* 使用 64 位临时变量，防止 delay_ms * 2 在比较上限前发生溢出。 */
    uint64_t next_delay = (uint64_t)delay_ms * 2U;
    if (next_delay > CONFIG_NETWORK_WIFI_RECONNECT_MAX_BACKOFF_MS) {
        next_delay = CONFIG_NETWORK_WIFI_RECONNECT_MAX_BACKOFF_MS;
    }
    s_reconnect_delay_ms = (uint32_t)next_delay;
}

/**
 * @brief 重连定时器回调。
 *
 * esp_timer 回调运行在 ESP Timer 任务中，不能执行长时间阻塞操作。
 * esp_wifi_connect() 本身为异步请求，因此可以安全调用。
 */
static void reconnect_timer_callback(void *argument)
{
    (void)argument;

    if (!s_started || s_stopping || network_has_ip()) return;

    ESP_LOGI(TAG, "Attempting Wi-Fi reconnect");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        schedule_reconnect();
    }
}

/**
 * @brief 统一处理 STA 断线。
 *
 * 只有设备此前确实已经获得 IP 时才发布 DISCONNECTED，避免首次连接失败
 * 过程中反复向 MQTT 等上层组件发送无意义的“掉线”事件。
 */
static void handle_disconnected(const wifi_event_sta_disconnected_t *event)
{
    bool was_connected = network_has_ip();

    /* 一旦无线连接断开，原 DHCP 地址不再被视为可用。 */
    xEventGroupClearBits(s_network_event_group, NETWORK_CONNECTED_BIT);

    if (event != NULL) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
    } else {
        ESP_LOGW(TAG, "Wi-Fi disconnected");
    }

    if (was_connected) {
        publish_network_event(NETWORK_EVENT_DISCONNECTED);
    }

    schedule_reconnect();
}

/**
 * @brief ESP-IDF Wi-Fi/IP 事件处理器。
 *
 * 处理的状态转换如下：
 * WIFI_EVENT_STA_START        -> 发起首次连接；
 * WIFI_EVENT_STA_DISCONNECTED -> 清除状态并安排指数退避重连；
 * IP_EVENT_STA_GOT_IP         -> 标记联网、停止重连并通知上层。
 */
static void event_handler(void *argument,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* 每次重新启动 Wi-Fi 都从 1 秒退避重新开始。 */
        s_reconnect_delay_ms = NETWORK_RECONNECT_INITIAL_DELAY_MS;

        /* 连接为异步操作，最终结果通过 GOT_IP 或 DISCONNECTED 事件返回。 */
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial esp_wifi_connect failed: %s", esp_err_to_name(err));
            schedule_reconnect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        handle_disconnected((const wifi_event_sta_disconnected_t *)event_data);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        bool was_connected = network_has_ip();

        /* 网络恢复成功：取消待执行重连，并重置下一轮的初始退避时间。 */
        stop_reconnect_timer();
        s_reconnect_delay_ms = NETWORK_RECONNECT_INITIAL_DELAY_MS;
        xEventGroupSetBits(s_network_event_group, NETWORK_CONNECTED_BIT);

        ESP_LOGI(TAG, "Connected to SSID \"%s\", IPv4=" IPSTR, CONFIG_NETWORK_WIFI_SSID, IP2STR(&event->ip_info.ip));

        /* DHCP 续租可能重复产生 GOT_IP，只在状态真正变化时通知上层。 */
        if (!was_connected) publish_network_event(NETWORK_EVENT_CONNECTED);
    }
}

/**
 * @brief 回收 network_init() 执行到一半时已经创建的资源。
 *
 * 按照“事件处理器 -> Wi-Fi 驱动 -> 定时器 -> netif -> Event Group”的
 * 逆初始化顺序清理，保证初始化失败后不会留下悬空回调或资源泄漏。
 */
static void cleanup_failed_init(bool wifi_initialized)
{
    if (s_ip_event_instance != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }

    if (s_wifi_event_instance != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }

    if (wifi_initialized) {
        esp_wifi_deinit();
    }

    if (s_reconnect_timer != NULL) {
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }

    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_network_event_group != NULL) {
        vEventGroupDelete(s_network_event_group);
        s_network_event_group = NULL;
    }
}

/**
 * @brief 初始化 Wi-Fi STA 组件，但不启动连接。
 */
esp_err_t network_init(void)
{
    /* 允许重复调用，避免上层初始化流程意外执行两次时重复创建驱动。 */
    if (s_initialized) return ESP_OK;

    /* 空 SSID 是明确的配置错误，但保持系统继续运行，方便重新配置。 */
    if (CONFIG_NETWORK_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG,
                 "Wi-Fi SSID is empty; configure it in "
                 "Network Service Configuration");
        return ESP_ERR_INVALID_ARG;
    }

    /* Event Group 用于跨任务等待“已获得 IP”这一状态。 */
    s_network_event_group = xEventGroupCreate();
    if (s_network_event_group == NULL) return ESP_ERR_NO_MEM;

    /* 创建默认 STA netif，并启用 ESP-IDF 默认 DHCP 客户端。 */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        cleanup_failed_init(false);
        return ESP_FAIL;
    }

    /* 创建一次性定时器；每次断线时根据当前退避时间重新启动。 */
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&reconnect_timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        cleanup_failed_init(false);
        return err;
    }

    /* 使用 ESP-IDF 推荐默认参数初始化 Wi-Fi 驱动。 */
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_config);
    if (err != ESP_OK) {
        cleanup_failed_init(false);
        return err;
    }
    bool wifi_initialized = true;

    /* 监听全部 Wi-Fi 事件，当前处理 STA_START 和 STA_DISCONNECTED。 */
    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        event_handler,
        NULL,
        &s_wifi_event_instance);
    if (err != ESP_OK) {
        cleanup_failed_init(wifi_initialized);
        return err;
    }

    /* 只监听 STA 获得 IPv4 地址事件。 */
    err = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        event_handler,
        NULL,
        &s_ip_event_instance);
    if (err != ESP_OK) {
        cleanup_failed_init(wifi_initialized);
        return err;
    }

    /*
     * wifi_config_t 中的 SSID、密码字段是固定长度数组，因此使用 strlcpy
     * 并显式提供目标大小，保证字符串始终以 '\0' 结尾。
     */
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid,
            CONFIG_NETWORK_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            CONFIG_NETWORK_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

    /* 设置允许连接的最低认证等级以及 WPA3 SAE 兼容方式。 */
    wifi_config.sta.threshold.authmode = NETWORK_AUTH_MODE_THRESHOLD;
    wifi_config.sta.sae_pwe_h2e = NETWORK_SAE_PWE_MODE;
    strlcpy((char *)wifi_config.sta.sae_h2e_identifier,
            CONFIG_NETWORK_WIFI_SAE_H2E_IDENTIFIER,
            sizeof(wifi_config.sta.sae_h2e_identifier));
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
    wifi_config.sta.disable_wpa3_compatible_mode = 0;
#endif

    /*
     * 凭据来源固定为 Kconfig，因此使用 WIFI_STORAGE_RAM。
     * 这样 Wi-Fi 驱动不会在自己的 NVS 配置中残留另一份旧 SSID/密码；
     * 每次启动都以当前固件 sdkconfig 中的配置为准。
     */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        cleanup_failed_init(wifi_initialized);
        return err;
    }

    /* 至此所有初始化步骤均成功，可以正式对外标记为 initialized。 */
    s_reconnect_delay_ms = NETWORK_RECONNECT_INITIAL_DELAY_MS;
    s_stopping = false;
    s_initialized = true;

    ESP_LOGI(TAG, "Wi-Fi STA service initialized for SSID \"%s\"", CONFIG_NETWORK_WIFI_SSID);
    return ESP_OK;
}

/**
 * @brief 启动 Wi-Fi 驱动并关闭省电模式。
 */
esp_err_t network_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;

    /* 在 esp_wifi_start() 前置位，确保 STA_START 回调可以立即发起连接。 */
    s_stopping = false;
    s_started = true;

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        s_started = false;
        return err;
    }

    /*
     * 当前项目优先保证 MQTT/OTA 实时性，因此关闭 Wi-Fi Modem Sleep。
     * 后续做低功耗优化时可以将此处改为 WIFI_PS_MIN_MODEM。
     */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable Wi-Fi power save: %s", esp_err_to_name(err));
        esp_wifi_stop();
        s_started = false;
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi STA service started");
    return ESP_OK;
}

/**
 * @brief 等待获得 IPv4 地址，超时后不停止后台重连。
 */
esp_err_t network_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized || !s_started) return ESP_ERR_INVALID_STATE;

    /* UINT32_MAX 作为公共 API 的“永久等待”特殊值。 */
    TickType_t timeout_ticks = timeout_ms == UINT32_MAX
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(
        s_network_event_group,
        NETWORK_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        timeout_ticks);

    return (bits & NETWORK_CONNECTED_BIT) != 0
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

/* 对外连接状态统一以 NETWORK_CONNECTED_BIT 为准。 */
bool network_is_connected(void)
{
    return network_has_ip();
}

/**
 * @brief 将 esp_netif 中保存的 IPv4 地址格式化为点分十进制字符串。
 */
esp_err_t network_get_ip(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) return ESP_ERR_INVALID_ARG;
    if (!network_has_ip() || s_sta_netif == NULL) return ESP_ERR_INVALID_STATE;

    /* 从默认 STA netif 读取 DHCP 当前分配的地址、掩码和网关。 */
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) return err;

    /* IPSTR/IP2STR 是 ESP-IDF 提供的 IPv4 安全格式化宏。 */
    int written = snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    if (written < 0 || (size_t)written >= buffer_size) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

/**
 * @brief 读取当前已连接 AP 的 RSSI。
 */
esp_err_t network_get_rssi(int8_t *rssi)
{
    if (rssi == NULL) return ESP_ERR_INVALID_ARG;
    if (!network_has_ip()) return ESP_ERR_INVALID_STATE;

    /* AP 记录中还包含 BSSID、信道和认证模式，目前只向上层返回 RSSI。 */
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) return err;

    *rssi = ap_info.rssi;
    return ESP_OK;
}

/**
 * @brief 手动触发一次重新连接。
 */
esp_err_t network_reconnect(void)
{
    if (!s_initialized || !s_started) return ESP_ERR_INVALID_STATE;

    /* 手动重连是一轮新连接过程，因此取消旧定时器并从 1 秒退避开始。 */
    stop_reconnect_timer();
    s_reconnect_delay_ms = NETWORK_RECONNECT_INITIAL_DELAY_MS;

    if (network_has_ip()) {
        /*
         * 已连接时只请求断开。随后产生的 STA_DISCONNECTED 事件会统一负责
         * 清除状态和安排下一次连接，避免这里与事件回调重复发起连接。
         */
        return esp_wifi_disconnect();
    }

    /* 当前未联网时先立即尝试；同步失败则退回定时重连状态机。 */
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) schedule_reconnect();
    return err;
}

/**
 * @brief 主动停止 Wi-Fi，但保留初始化资源以支持再次启动。
 */
esp_err_t network_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!s_started) return ESP_OK;

    bool was_connected = network_has_ip();

    /*
     * 必须先设置 stopping，再调用 esp_wifi_stop()。这样停止过程中产生的
     * DISCONNECTED 事件不会误认为异常掉线并再次安排重连。
     */
    s_stopping = true;
    stop_reconnect_timer();
    xEventGroupClearBits(s_network_event_group, NETWORK_CONNECTED_BIT);

    esp_err_t err = esp_wifi_stop();
    s_started = false;

    /* 主动停止同样会使上层网络不可用，因此补发一次状态变化事件。 */
    if (was_connected) publish_network_event(NETWORK_EVENT_DISCONNECTED);

    return err;
}
