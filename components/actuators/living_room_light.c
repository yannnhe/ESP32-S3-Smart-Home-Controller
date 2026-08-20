#include "living_room_light.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "adc.h"
#include "ha_discovery.h"
#include "ha_mqtt.h"

#define LIVING_ROOM_LIGHT_GPIO               GPIO_NUM_11 /**< WS2812B 数据输出 GPIO。 */
#define LIVING_ROOM_BUTTON_GPIO              GPIO_NUM_14 /**< 本地按键输入 GPIO。 */
#define LIVING_ROOM_LIGHT_LED_COUNT          43 /**< 灯带中的 WS2812B 总数。 */
#define LIVING_ROOM_BUTTON_DEBOUNCE_MS       50U /**< 按键按下后的软件去抖时间。 */
#define LIVING_ROOM_DEFAULT_BRIGHTNESS       128U /**< 首次开启灯带时使用的默认亮度（0–255）。 */
#define LIVING_ROOM_LIGHT_QUEUE_LENGTH       8 /**< 各输入源共用的命令队列长度。 */
#define LIVING_ROOM_LIGHT_COMMAND_TOPIC      "smarthome/esp32-1/light/living-room/set" /**< HA 下发控制命令的 MQTT 主题。 */

/** 灯带执行任务可处理的两类内部命令。 */
typedef enum {
    LIVING_ROOM_LIGHT_COMMAND_TOGGLE, /**< 反转当前开关状态。 */
    LIVING_ROOM_LIGHT_COMMAND_SET, /**< 设置指定开关状态和/或亮度。 */
} living_room_light_command_type_t;

/** 由 HA、按键或旋钮投递给灯带执行任务的一条命令。 */
typedef struct {
    living_room_light_command_type_t type; /**< 命令类型。 */
    bool has_state; /**< 是否携带 is_on 字段。 */
    bool is_on; /**< 期望的开关状态。 */
    bool has_brightness; /**< 是否携带 brightness 字段。 */
    uint8_t brightness; /**< 期望亮度，范围为 0–255。 */
} living_room_light_command_t;

/** 灯带实际已成功写入硬件、可安全回传给 HA 的状态缓存。 */
typedef struct {
    bool is_on; /**< 当前实际开关状态。 */
    uint8_t brightness; /**< 当前逻辑亮度，范围为 0–255。 */
    uint8_t last_nonzero_brightness; /**< 最近一次非零亮度，供重新打开时恢复。 */
} living_room_light_state_t;

static const char *TAG = "living_room_light"; /**< 本组件的日志标签。 */
static led_strip_handle_t s_led_strip; /**< RMT WS2812B 灯带驱动句柄。 */
static QueueHandle_t s_command_queue; /**< 汇集 HA、按键、旋钮命令的 FreeRTOS 队列。 */
static TaskHandle_t s_light_task; /**< 串行执行所有灯带命令的任务句柄。 */
static TaskHandle_t s_button_task; /**< 负责按键去抖与释放等待的任务句柄。 */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED; /**< 保护 s_state 与旋钮基线标志的临界区锁。 */
static living_room_light_state_t s_state = { /**< 当前已实际写入灯带的状态缓存。 */
    .is_on = false, /**< 上电默认关闭。 */
    .brightness = LIVING_ROOM_DEFAULT_BRIGHTNESS, /**< 关闭前预置默认亮度。 */
    .last_nonzero_brightness = LIVING_ROOM_DEFAULT_BRIGHTNESS, /**< 首次打开时可恢复的亮度。 */
};
static bool s_knob_baselined; /**< 是否已忽略旋钮启动时的初始位置。 */
static bool s_initialized; /**< 是否已完成本组件初始化，防止重复创建资源。 */
static ha_discovery_state_group_handle_t s_state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE; /**< 灯带 MQTT 状态组句柄。 */

/**
 * 将当前灯带缓存编码为 MQTT Light JSON 状态。
 * @param payload 输出 JSON 缓冲区。
 * @param payload_size 输出缓冲区大小。
 * @param context 未使用的状态组上下文。
 */
static esp_err_t encode_light_state(char *payload, size_t payload_size, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    const bool is_on = s_state.is_on; /**< 锁内复制的实际开关状态。 */
    const uint8_t brightness = s_state.brightness; /**< 锁内复制的实际亮度。 */
    portEXIT_CRITICAL(&s_state_lock);

    const int written = snprintf(payload, payload_size,
                                 "{\"state\":\"%s\",\"brightness\":%u}",
                                 is_on ? "ON" : "OFF", brightness);
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/**
 * 将目标状态实际写入 WS2812B 灯带。
 * @param is_on 目标开关状态。
 * @param brightness 目标白光亮度，范围为 0–255。
 */
static esp_err_t write_light_output(bool is_on, uint8_t brightness)
{
    if (!is_on) return led_strip_clear(s_led_strip);

    /* i 依次遍历灯带上的每一颗 LED。 */
    for (size_t i = 0; i < LIVING_ROOM_LIGHT_LED_COUNT; ++i) {
        esp_err_t err = led_strip_set_pixel(s_led_strip, i, brightness, brightness, brightness); /**< 当前像素设置结果。 */
        if (err != ESP_OK) return err;
    }
    return led_strip_refresh(s_led_strip);
}

/** 请求 Discovery 发布当前缓存的 retained 灯带状态。 */
static void publish_current_state(void)
{
    esp_err_t err = ha_discovery_publish_state_group(s_state_group); /**< 异步发布请求的返回结果。 */
    if (err != ESP_OK) ESP_LOGW(TAG, "Light state publish request failed: %s", esp_err_to_name(err));
}

/**
 * 在灯带执行任务中应用一条命令；硬件刷新成功后才提交缓存并回传 HA。
 * @param command 已从命令队列取出的控制命令。
 */
static void apply_command(const living_room_light_command_t *command)
{
    portENTER_CRITICAL(&s_state_lock);
    living_room_light_state_t target = s_state; /**< 基于当前缓存计算出的目标状态副本。 */
    portEXIT_CRITICAL(&s_state_lock);

    if (command->type == LIVING_ROOM_LIGHT_COMMAND_TOGGLE) {
        target.is_on = !target.is_on;
        if (target.is_on && target.brightness == 0) target.brightness = target.last_nonzero_brightness;
    } else {
        if (command->has_brightness) {
            target.brightness = command->brightness;
            if (command->brightness > 0) target.last_nonzero_brightness = command->brightness;
        }
        if (command->has_state) target.is_on = command->is_on;
        else if (command->has_brightness) target.is_on = command->brightness > 0;
        if (target.is_on && target.brightness == 0) target.brightness = target.last_nonzero_brightness;
    }

    esp_err_t err = write_light_output(target.is_on, target.brightness); /**< WS2812B 实际刷新结果。 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 output update failed: %s", esp_err_to_name(err));
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state = target;
    portEXIT_CRITICAL(&s_state_lock);
    publish_current_state();
}

/**
 * 非阻塞地向灯带执行任务投递命令。
 * @param command 待投递的命令。
 */
static esp_err_t enqueue_command(const living_room_light_command_t *command)
{
    if (s_command_queue == NULL || command == NULL) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_command_queue, command, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * 比较一段非 '\0' 结尾的 MQTT 载荷与预期文本。
 * @param payload MQTT 回调提供的载荷起始地址。
 * @param payload_length 载荷长度。
 * @param expected 以 '\0' 结尾的预期文本。
 */
static bool payload_equals(const char *payload, size_t payload_length, const char *expected)
{
    const size_t expected_length = strlen(expected); /**< 预期文本长度。 */
    return payload_length == expected_length && memcmp(payload, expected, expected_length) == 0;
}

/**
 * 解析 HA 的 MQTT Light 命令；只做快速校验与入队，不直接操作硬件。
 * @param payload MQTT 命令载荷，可为 ON/OFF 或 JSON。
 * @param payload_length MQTT 命令载荷长度。
 * @param context 未使用的注册上下文。
 */
static esp_err_t handle_mqtt_command(const char *payload, size_t payload_length, void *context)
{
    (void)context;
    living_room_light_command_t command = { /**< 由 MQTT 载荷解析出的内部命令。 */
        .type = LIVING_ROOM_LIGHT_COMMAND_SET,
    };

    if (payload_equals(payload, payload_length, "ON")) {
        command.has_state = true;
        command.is_on = true;
        return enqueue_command(&command);
    }
    if (payload_equals(payload, payload_length, "OFF")) {
        command.has_state = true;
        command.is_on = false;
        return enqueue_command(&command);
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_length); /**< JSON 根节点，解析后必须释放。 */
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state"); /**< 可选的 ON/OFF JSON 字段。 */
    if (cJSON_IsString(state) && state->valuestring != NULL) {
        if (strcmp(state->valuestring, "ON") == 0) {
            command.has_state = true;
            command.is_on = true;
        } else if (strcmp(state->valuestring, "OFF") == 0) {
            command.has_state = true;
            command.is_on = false;
        } else {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness"); /**< 可选的 0–255 亮度 JSON 字段。 */
    if (cJSON_IsNumber(brightness)) {
        if (brightness->valuedouble < 0.0 || brightness->valuedouble > 255.0) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        command.has_brightness = true;
        command.brightness = (uint8_t)lround(brightness->valuedouble);
    }

    cJSON_Delete(root);
    if (!command.has_state && !command.has_brightness) return ESP_ERR_INVALID_ARG;
    return enqueue_command(&command);
}

/**
 * 灯带唯一执行任务：串行取出命令并完成硬件、缓存和 HA 状态闭环。
 * @param argument 未使用的 FreeRTOS 任务参数。
 */
static void light_command_task(void *argument)
{
    (void)argument;

    while (true) {
        living_room_light_command_t command; /**< 从共享队列取出的下一条命令。 */
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            apply_command(&command);
        }
    }
}

/**
 * 按键任务：接收中断通知、去抖、确认按下并投递一次切换命令。
 * @param argument 未使用的 FreeRTOS 任务参数。
 */
static void button_task(void *argument)
{
    (void)argument;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(LIVING_ROOM_BUTTON_DEBOUNCE_MS));
        if (gpio_get_level(LIVING_ROOM_BUTTON_GPIO) == 0) continue;

        const living_room_light_command_t command = { /**< 已去抖确认后的本地切换命令。 */
            .type = LIVING_ROOM_LIGHT_COMMAND_TOGGLE,
        };
        esp_err_t err = enqueue_command(&command); /**< 本地按键命令的入队结果。 */
        if (err != ESP_OK) ESP_LOGW(TAG, "Local button command dropped: %s", esp_err_to_name(err));

        while (gpio_get_level(LIVING_ROOM_BUTTON_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// GPIO14 上升沿 ISR：仅唤醒按键任务，避免在中断上下文中执行耗时操作。
static void IRAM_ATTR button_gpio_isr(void *argument)
{
    (void)argument;
    BaseType_t higher_priority_task_woken = pdFALSE; /**< 是否唤醒了更高优先级任务。 */
    vTaskNotifyGiveFromISR(s_button_task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) portYIELD_FROM_ISR();
}

/**
 * GPIO1 旋钮监听回调：忽略首次基线值，后续将百分比映射为灯带开关和亮度命令。
 * @param percent ADC 组件提供的旋钮百分比。
 * @param context 未使用的监听器上下文。
 */
static void knob_listener(float percent, void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_knob_baselined) {
        s_knob_baselined = true;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    portEXIT_CRITICAL(&s_state_lock);

    living_room_light_command_t command = { /**< 由旋钮百分比换算出的灯带设置命令。 */
        .type = LIVING_ROOM_LIGHT_COMMAND_SET,
        .has_state = true,
        .is_on = percent > 1.0f,
    };
    if (command.is_on) {
        command.has_brightness = true;
        command.brightness = (uint8_t)lroundf(percent / 100.0f * 255.0f);
        if (command.brightness == 0) command.brightness = 1;
    }

    esp_err_t err = enqueue_command(&command); /**< 旋钮命令的入队结果。 */
    if (err != ESP_OK) ESP_LOGW(TAG, "Knob command dropped: %s", esp_err_to_name(err));
}

/** 尝试读取 ADC 缓存并建立旋钮基线，防止初始化时旋钮位置直接改变灯带。 */
static void establish_knob_baseline(void)
{
    float percent; /**< ADC 缓存的当前旋钮百分比，仅用于判断是否已有有效采样。 */
    if (adc_get_knob_percent(&percent) != ESP_OK) return;

    portENTER_CRITICAL(&s_state_lock);
    s_knob_baselined = true;
    portEXIT_CRITICAL(&s_state_lock);
}

/**
 * 初始化 WS2812B、命令任务、本地按键、HA MQTT Light 和 ADC 旋钮监听。
 * @return ESP_OK 表示所有资源均已创建；其他值表示首个失败步骤的错误码。
 */
esp_err_t living_room_light_init(void)
{
    if (s_initialized) return ESP_OK;

    const led_strip_config_t strip_config = { /**< WS2812B 灯带型号、数量、色序和数据 GPIO 配置。 */
        .strip_gpio_num = LIVING_ROOM_LIGHT_GPIO,
        .max_leds = LIVING_ROOM_LIGHT_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = { /**< RMT 时钟与传输方式配置。 */
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip); /**< 当前初始化或注册步骤的返回结果。 */
    if (err != ESP_OK) return err;

    err = write_light_output(false, 0);
    if (err != ESP_OK) return err;

    s_command_queue = xQueueCreate(LIVING_ROOM_LIGHT_QUEUE_LENGTH, sizeof(living_room_light_command_t));
    if (s_command_queue == NULL) return ESP_ERR_NO_MEM;

    const ha_discovery_state_group_config_t state_group_config = { /**< 灯带 retained JSON 状态主题的配置。 */
        .state_key = "light/living-room",
        .encode_payload = encode_light_state,
        .context = NULL,
    };
    err = ha_discovery_register_state_group(&state_group_config, &s_state_group);
    if (err != ESP_OK) return err;

    const ha_discovery_light_config_t discovery_config = { /**< HA MQTT Light Discovery 实体配置。 */
        .entity_key = "living-room-light",
        .name = "WS2812B Light - Living Room",
        .command_key = "light/living-room",
        .state_group = s_state_group,
        .supports_brightness = true,
        .include_full_device_info = true,
    };
    ha_discovery_light_handle_t ignored_light; /**< 当前无需保存的 Light Discovery 实体句柄。 */
    err = ha_discovery_register_light(&discovery_config, &ignored_light);
    if (err != ESP_OK) return err;

    if (xTaskCreate(light_command_task, "living_room_light", 4096, NULL, tskIDLE_PRIORITY + 1, &s_light_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(button_task, "living_room_button", 3072, NULL, tskIDLE_PRIORITY + 1, &s_button_task) != pdPASS) return ESP_ERR_NO_MEM;

    const gpio_config_t button_gpio_config = { /**< GPIO14 高有效按键的输入与上升沿中断配置。 */
        .pin_bit_mask = 1ULL << LIVING_ROOM_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    err = gpio_config(&button_gpio_config);
    if (err != ESP_OK) return err;

    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = gpio_isr_handler_add(LIVING_ROOM_BUTTON_GPIO, button_gpio_isr, NULL);
    if (err != ESP_OK) return err;

    err = ha_mqtt_register_command_handler(LIVING_ROOM_LIGHT_COMMAND_TOPIC, handle_mqtt_command, NULL);
    if (err != ESP_OK) return err;

    err = adc_register_knob_listener(knob_listener, NULL);
    if (err != ESP_OK) return err;
    establish_knob_baseline();

    s_initialized = true;
    ESP_LOGI(TAG, "Living room WS2812B light initialized: GPIO11, 43 LEDs, GRB");
    return ESP_OK;
}
