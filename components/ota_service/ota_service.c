#include "ota_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ha_discovery.h"
#include "ha_mqtt.h"
#include "network.h"

#define OTA_COMMAND_TOPIC             "smarthome/esp32-1/ota/set"
#define OTA_COMMAND_PAYLOAD           "START"
#define OTA_READ_BUFFER_SIZE          1024U
#define OTA_TASK_STACK_SIZE           8192U
#define OTA_WORK_QUEUE_LENGTH         4U
#define OTA_RESTART_STATUS_WAIT_MS    500U

// 固件开头用于校验的最小字节数。其包含： ESP 镜像头→ 魔数、芯片型号等    第一个段头→ 段信息                    应用描述 esp_app_desc_t → PROJECT_VER、项目名、构建时间等
#define OTA_IMAGE_HEADER_SIZE         (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

typedef enum {
    OTA_WORK_START,
    OTA_WORK_HEALTH_CHANGED,
    OTA_WORK_ROLLBACK,
} ota_work_type_t;

static const char *TAG = "ota_service";
static QueueHandle_t s_work_queue;
static TaskHandle_t s_worker_task;
static esp_timer_handle_t s_health_timer;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_busy; // 为 true 时 HA OTA Button 的 availability 会发布为 offline
static bool s_pending_verification; // 当前运行镜像是否处于 ESP-IDF 的 PENDING_VERIFY 状态。只有新镜像会进入该状态。
static bool s_network_ready;
static bool s_mqtt_ready;
static bool s_discovery_ready;
static char s_status[40] = "idle"; // 要发布到 HA OTA Status Sensor 的文本状态
static ha_discovery_state_group_handle_t s_status_state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE;
static ha_discovery_state_group_handle_t s_button_availability_state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE;
static uint8_t s_download_buffer[OTA_READ_BUFFER_SIZE]; // 1 KiB HTTP 下载缓冲区（静态全局内存）
static uint8_t s_image_header[OTA_IMAGE_HEADER_SIZE]; // 暂存首段镜像信息

/** 状态文本传给 HA 的 Sensor；状态仅由 OTA 任务或快速命令回调更新。 */
static esp_err_t encode_status(char *payload, size_t payload_size, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    const int written = snprintf(payload, payload_size, "%s", s_status);
    portEXIT_CRITICAL(&s_state_lock);
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/** 下载进行中时让 Button 专用 availability 离线，以禁止重复按下。 */
static esp_err_t encode_button_availability(char *payload, size_t payload_size, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    const bool busy = s_busy;
    portEXIT_CRITICAL(&s_state_lock);
    if (busy) return ESP_ERR_INVALID_STATE;

    const int written = snprintf(payload, payload_size, "ready");
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void set_status(const char *status)
{
    portENTER_CRITICAL(&s_state_lock);
    snprintf(s_status, sizeof(s_status), "%s", status);
    portEXIT_CRITICAL(&s_state_lock);
    if (s_status_state_group != HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE) {
        (void)ha_discovery_publish_state_group(s_status_state_group);
    }
}

static void set_busy(bool busy)
{
    portENTER_CRITICAL(&s_state_lock);
    s_busy = busy;
    portEXIT_CRITICAL(&s_state_lock);
    if (s_button_availability_state_group != HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE) {
        (void)ha_discovery_publish_state_group(s_button_availability_state_group);
    }
}

/** URL 不来自 MQTT，仍严格限制 scheme 和 host，避免本地配置误指向其他站点。 */
static bool is_allowed_firmware_url(const char *url)
{
    static const char allowed_prefix[] = "http://bin.bemfa.com/";
    if (url == NULL || strncmp(url, allowed_prefix, sizeof(allowed_prefix) - 1) != 0
        || url[sizeof(allowed_prefix) - 1] == '\0') {
        return false;
    }
    for (const char *cursor = url; *cursor != '\0'; ++cursor) {
        // 拒绝所有 ASCII 码小于等于 0x20（即空格、换行 \n、回车 \r、制表符 \t 等）的字符
        if ((unsigned char)*cursor <= 0x20U) return false;
    }
    return true;
}

/** 只接受无前导零的正整数版本，例如巴法云上传序号 1、2、3。 */
static bool parse_positive_integer_version(const char *text, uint32_t *version)
{
    if (text == NULL || version == NULL || text[0] == '\0' || text[0] == '0') return false;

    uint32_t value = 0;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *version = value;
    return true;
}

/** 比较候选云端镜像版本与当前运行镜像版本；正值表示候选版本更新。 */
static int compare_versions(const char *candidate, const char *running)
{
    uint32_t candidate_number = 0;
    uint32_t running_number = 0;
    if (!parse_positive_integer_version(candidate, &candidate_number)
        || !parse_positive_integer_version(running, &running_number)) return -2;
    if (candidate_number > running_number) return 1;
    if (candidate_number < running_number) return -1;
    return 0;
}

/** 检查首段镜像头、芯片型号和版本策略，且不写入 flash。 */
static const char *validate_image_header(const uint8_t *image, size_t image_size)
{
    if (image_size < OTA_IMAGE_HEADER_SIZE) return "failed_header";

    esp_image_header_t image_header;
    esp_app_desc_t candidate_description;
    memcpy(&image_header, image, sizeof(image_header));
    memcpy(&candidate_description, image + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(candidate_description));
    candidate_description.version[sizeof(candidate_description.version) - 1] = '\0';

    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC || image_header.chip_id != ESP_CHIP_ID_ESP32S3
        || candidate_description.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "failed_image_target";
    }

    const esp_app_desc_t *running_description = esp_app_get_description();
    if (running_description == NULL) return "failed_running_version";

    const int version_result = compare_versions(candidate_description.version, running_description->version);
    if (version_result == -2) return "failed_version_format";
    if (version_result <= 0) return "rejected_not_newer";

    // 曾回滚镜像保护
    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
    if (last_invalid != NULL) {
        esp_app_desc_t invalid_description;
        if (esp_ota_get_partition_description(last_invalid, &invalid_description) == ESP_OK) {
            invalid_description.version[sizeof(invalid_description.version) - 1] = '\0';
            if (strcmp(candidate_description.version, invalid_description.version) == 0) {
                return "rejected_invalid_version";
            }
        }
    }
    return NULL;
}

static void finish_failed_update(const char *status, esp_http_client_handle_t client,
                                 bool ota_started, esp_ota_handle_t update_handle)
{
    if (ota_started) (void)esp_ota_abort(update_handle);
    if (client != NULL) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    ESP_LOGW(TAG, "OTA request ended: %s", status);
    set_busy(false);
    set_status(status);
}

/** 在专用任务内完成下载、镜像验证、写入和重启，绝不阻塞 MQTT 回调。 */
static void perform_update(void)
{
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t update_handle = 0;
    bool ota_started = false;
    const char *failure_status = NULL;

    if (!network_is_connected()) {
        finish_failed_update("failed_network", NULL, false, 0);
        return;
    }
    if (!is_allowed_firmware_url(CONFIG_OTA_SERVICE_FIRMWARE_URL)) {
        finish_failed_update("failed_config", NULL, false, 0);
        return;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        finish_failed_update("failed_partition", NULL, false, 0);
        return;
    }

    const esp_http_client_config_t http_config = {
        .url = CONFIG_OTA_SERVICE_FIRMWARE_URL,
        .timeout_ms = CONFIG_OTA_SERVICE_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        finish_failed_update("failed_http_init", NULL, false, 0);
        return;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        failure_status = "failed_http_open";
        goto failed;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length <= (int64_t)OTA_IMAGE_HEADER_SIZE       // 下载内容不足基本镜像头
        || (uint64_t)content_length > update_partition->size   // 固件大于目标 OTA 分区
        || esp_http_client_get_status_code(client) != 200)     // 拒绝 404、403、500、重定向等非正常固件响应
    {
        failure_status = "failed_http_response";
        goto failed;
    }

    size_t header_length = 0;
    size_t written_length = 0;
    set_status("downloading");
    while (true) {
        const int bytes_read = esp_http_client_read(client, (char *)s_download_buffer,
                                                    sizeof(s_download_buffer));
        if (bytes_read < 0) {
            failure_status = "failed_download";
            goto failed;
        }
        if (bytes_read == 0) break;
        
        // 表示当前这一次 HTTP 数据块中，前面有多少字节已经被当成“镜像头的一部分”处理过。
        // 只有在最后一次镜像头的完整读取才起作用
        size_t offset = 0;

        if (!ota_started) {
            const size_t needed = OTA_IMAGE_HEADER_SIZE - header_length;

            // 表示当前数据块中有多少字节可用于补齐头部
            const size_t available = (size_t)bytes_read < needed ? (size_t)bytes_read : needed;

            memcpy(s_image_header + header_length, s_download_buffer, available);
            header_length += available;
            offset = available;
            if (header_length < OTA_IMAGE_HEADER_SIZE) continue;

            set_status("verifying");
            failure_status = validate_image_header(s_image_header, header_length);
            if (failure_status != NULL) goto failed;

            if (esp_ota_begin(update_partition, (size_t)content_length, &update_handle) != ESP_OK) {
                failure_status = "failed_ota_begin";
                goto failed;
            }
            ota_started = true;
            if (esp_ota_write(update_handle, s_image_header, header_length) != ESP_OK) {
                failure_status = "failed_ota_write";
                goto failed;
            }
            written_length += header_length;
        }

        if ((size_t)bytes_read > offset) {
            const size_t bytes_to_write = (size_t)bytes_read - offset;
            if (esp_ota_write(update_handle, s_download_buffer + offset, bytes_to_write) != ESP_OK) {
                failure_status = "failed_ota_write";
                goto failed;
            }
            written_length += bytes_to_write;
        }
    }

    if (!ota_started                                            // 已成功启动 OTA 写入
        || !esp_http_client_is_complete_data_received(client)   // HTTP 客户端确认完整文件已接收
        || written_length != (size_t)content_length)            // 实际写入字节数 == HTTP Content-Length
    {
        failure_status = "failed_incomplete";
        goto failed;
    }
    if (esp_ota_end(update_handle) != ESP_OK) {
        ota_started = false; /* esp_ota_end() 已释放该 handle。 */
        failure_status = "failed_image_validate";
        goto failed;
    }
    ota_started = false;
    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        failure_status = "failed_boot_partition";
        goto failed;
    }

    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    set_status("rebooting");
    vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_STATUS_WAIT_MS));
    esp_restart();
    return;

failed:
    // 失败处理：释放资源、记录日志、更新状态
    finish_failed_update(failure_status == NULL ? "failed_unknown" : failure_status, client, ota_started, update_handle);
}

/** 已处于 pending verify 的新镜像，只有三个健康条件全部满足才确认有效。 */
static void evaluate_pending_verification(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool healthy = s_pending_verification && s_network_ready && s_mqtt_ready && s_discovery_ready;
    portEXIT_CRITICAL(&s_state_lock);
    if (!healthy) return;

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_pending_verification = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (s_health_timer != NULL) (void)esp_timer_stop(s_health_timer);
        ESP_LOGI(TAG, "New OTA image passed Wi-Fi, MQTT and HA synchronization health checks");
        set_busy(false);
        set_status("idle");
    } else {
        ESP_LOGE(TAG, "Failed to mark new OTA image valid; waiting for health timeout rollback");
    }
}

static void health_timeout_callback(void *argument)
{
    (void)argument;
    const ota_work_type_t work = OTA_WORK_ROLLBACK;
    if (s_work_queue != NULL) (void)xQueueSend(s_work_queue, &work, 0);
}

static void ota_worker_task(void *argument)
{
    (void)argument;
    ota_work_type_t work;
    while (true) {
        if (xQueueReceive(s_work_queue, &work, portMAX_DELAY) != pdTRUE) continue;
        if (work == OTA_WORK_START) {
            /* MQTT 回调已抢占 busy 标志；由本任务实际发布禁用状态。 */
            set_busy(true);
            set_status("queued");
            perform_update();
        } else if (work == OTA_WORK_HEALTH_CHANGED) {
            evaluate_pending_verification();
        } else if (work == OTA_WORK_ROLLBACK) {
            portENTER_CRITICAL(&s_state_lock);
            const bool pending = s_pending_verification;
            portEXIT_CRITICAL(&s_state_lock);
            if (pending) {
                ESP_LOGE(TAG, "New OTA image did not become healthy before timeout; rolling back");
                set_status("rollback");
                vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_STATUS_WAIT_MS));
                if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to mark pending OTA image invalid for rollback");
                    set_status("failed_rollback");
                }
            }
        }
    }
}

static void queue_health_evaluation(void)
{
    const ota_work_type_t work = OTA_WORK_HEALTH_CHANGED;
    if (s_work_queue != NULL) (void)xQueueSend(s_work_queue, &work, 0);
}

static void network_event_handler(void *handler_args, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    (void)event_data;
    portENTER_CRITICAL(&s_state_lock);
    s_network_ready = event_id == NETWORK_EVENT_CONNECTED;
    portEXIT_CRITICAL(&s_state_lock);
    queue_health_evaluation();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    (void)event_data;
    if (event_id == HA_MQTT_EVENT_CONNECTED || event_id == HA_MQTT_EVENT_DISCONNECTED) {
        portENTER_CRITICAL(&s_state_lock);
        s_mqtt_ready = event_id == HA_MQTT_EVENT_CONNECTED;
        portEXIT_CRITICAL(&s_state_lock);
    }
    queue_health_evaluation();
}

static void discovery_event_handler(void *handler_args, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    (void)event_data;
    if (event_id != HA_DISCOVERY_EVENT_FULL_SYNC_COMPLETE) return;
    portENTER_CRITICAL(&s_state_lock);
    s_discovery_ready = true;
    portEXIT_CRITICAL(&s_state_lock);
    queue_health_evaluation();
}

/** MQTT 回调只负责精确校验 START 并投递工作；不执行 HTTP、写 flash 或 MQTT 发布。 */
static esp_err_t ota_command_handler(const char *payload, size_t payload_length, void *context)
{
    (void)context;
    if (payload_length != sizeof(OTA_COMMAND_PAYLOAD) - 1
        || memcmp(payload, OTA_COMMAND_PAYLOAD, sizeof(OTA_COMMAND_PAYLOAD) - 1) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
        以下情况拒绝新 OTA：
        1. 已经排队或正在下载
        2. 正在校验镜像
        3. 已完成下载、正等待重启
        4. 新镜像已启动但仍处于 PENDING_VERIFY
    */

    portENTER_CRITICAL(&s_state_lock);
    if (s_busy || s_pending_verification) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    portEXIT_CRITICAL(&s_state_lock);

    const ota_work_type_t work = OTA_WORK_START;

    // 队列创建失败或已满时，立即清除 busy 标志并返回错误
    if (s_work_queue == NULL || xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_service_init(void)
{
    if (s_initialized) return ESP_OK;

    const ha_discovery_state_group_config_t status_group_config = {
        .state_key = "diagnostic/ota-status",
        .encode_payload = encode_status,
        .context = NULL,
    };
    esp_err_t err = ha_discovery_register_state_group(&status_group_config, &s_status_state_group);
    if (err != ESP_OK) return err;

    const ha_discovery_sensor_config_t status_sensor_config = {
        .entity_key = "ota-status",
        .name = "OTA Status",
        .device_class = NULL,
        .unit_of_measurement = NULL,
        .entity_category = "diagnostic",
        .value_template = NULL,
        .state_class = "",
        .state_group = s_status_state_group,
        .include_full_device_info = false,
    };
    ha_discovery_sensor_handle_t status_sensor;
    err = ha_discovery_register_sensor(&status_sensor_config, &status_sensor);
    if (err != ESP_OK) return err;

    const ha_discovery_state_group_config_t button_group_config = {
        .state_key = "diagnostic/ota-button",
        .encode_payload = encode_button_availability,
        .context = NULL,
    };
    err = ha_discovery_register_state_group(&button_group_config, &s_button_availability_state_group);
    if (err != ESP_OK) return err;

    const ha_discovery_button_config_t button_config = {
        .entity_key = "ota-start",
        .name = "Start OTA",
        .command_key = "ota",
        .availability_state_group = s_button_availability_state_group,
        .include_full_device_info = false,
    };
    ha_discovery_button_handle_t button;
    err = ha_discovery_register_button(&button_config, &button);
    if (err != ESP_OK) return err;

    s_work_queue = xQueueCreate(OTA_WORK_QUEUE_LENGTH, sizeof(ota_work_type_t));
    if (s_work_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ota_worker_task, "ota_service", OTA_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, &s_worker_task) != pdPASS) {
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = ha_mqtt_register_command_handler(OTA_COMMAND_TOPIC, ota_command_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(NETWORK_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(HA_MQTT_EVENT, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(HA_DISCOVERY_EVENT, HA_DISCOVERY_EVENT_FULL_SYNC_COMPLETE, discovery_event_handler, NULL);
    if (err != ESP_OK) return err;

    // 返回当前代码实际运行所在的应用分区
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state;

    /*
        判断：
        1. 能否找到当前运行的应用分区
        2. 能否成功找到当前运行分区的 OTA 状态且 image_state 已被正确填入
        3. 当前运行分区的 OTA 状态是否为 ESP_OTA_IMG_PENDING_VERIFY
    */
    if (running_partition != NULL 
        && esp_ota_get_state_partition(running_partition, &image_state) == ESP_OK 
        && image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        portENTER_CRITICAL(&s_state_lock);
        s_pending_verification = true;
        portEXIT_CRITICAL(&s_state_lock);
        /* 待确认镜像期间也不允许再启动另一轮 OTA。 */
        set_busy(true);
        set_status("verifying");

        const esp_timer_create_args_t timer_args = {
            .callback = health_timeout_callback,
            .name = "ota_health",
        };
        err = esp_timer_create(&timer_args, &s_health_timer);
        if (err != ESP_OK) return err;
        err = esp_timer_start_once(s_health_timer, (uint64_t)CONFIG_OTA_SERVICE_HEALTH_TIMEOUT_SECONDS * 1000000ULL);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "Pending OTA image must complete health checks within %d seconds", CONFIG_OTA_SERVICE_HEALTH_TIMEOUT_SECONDS);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OTA service initialized; firmware URL is configured only through menuconfig");
    return ESP_OK;
}
