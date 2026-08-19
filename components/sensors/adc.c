#include "adc.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ha_discovery.h"

#define ADC_SENSOR_COUNT                  7
#define ADC_MEDIAN_SAMPLE_COUNT           5
#define ADC_FAST_SAMPLE_INTERVAL_MS       200U
#define ADC_SLOW_SAMPLE_INTERVAL_MS       5000U

#define ADC_KNOB_PUBLISH_DEADBAND_PERCENT 1.0f

typedef enum {
    ADC_CONVERSION_KNOB_PERCENT,
    ADC_CONVERSION_MQ2_PPM,
    ADC_CONVERSION_UV_INDEX,
    ADC_CONVERSION_MQ135_PPM,
    ADC_CONVERSION_ILLUMINANCE_LUX,
    ADC_CONVERSION_RAIN_PERCENT,
    ADC_CONVERSION_WATER_LEVEL_PERCENT,
} adc_conversion_t;

typedef struct {
    const char *state_key;
    const char *entity_key;
    const char *name;
    const char *device_class;
    const char *unit_of_measurement;
    adc_channel_t channel;
    adc_atten_t attenuation;
    adc_conversion_t conversion;
    bool fast_sample;
    bool include_full_device_info;
    bool valid;
    bool unavailable_reported;
    bool has_published_value;
    float value;
    float last_published_value;
    ha_discovery_state_group_handle_t state_group;
} adc_sensor_t;

static const char *TAG = "adc";

static adc_sensor_t s_adc_sensors[ADC_SENSOR_COUNT] = {
    {
        .state_key = "living-room/brightness-knob",
        .entity_key = "living-room-brightness-knob",
        .name = "Living Room Brightness Knob",
        .device_class = NULL,
        .unit_of_measurement = "%",
        .channel = ADC_CHANNEL_0,
        .attenuation = ADC_ATTEN_DB_12,
        .conversion = ADC_CONVERSION_KNOB_PERCENT,
        .fast_sample = true,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .state_key = "kitchen/mq2",
        .entity_key = "kitchen-flammable-gas",
        .name = "Flammable Gas - Kitchen",
        .device_class = NULL,
        .unit_of_measurement = "ppm",
        .channel = ADC_CHANNEL_1,
        .attenuation = ADC_ATTEN_DB_12,
        .conversion = ADC_CONVERSION_MQ2_PPM,
        .fast_sample = false,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .state_key = "balcony/uv-index",
        .entity_key = "balcony-uv-index",
        .name = "UV Index - Balcony",
        .device_class = "uv_index",
        .unit_of_measurement = NULL,
        .channel = ADC_CHANNEL_3,
        .attenuation = ADC_ATTEN_DB_0,
        .conversion = ADC_CONVERSION_UV_INDEX,
        .fast_sample = false,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .state_key = "balcony/mq135",
        .entity_key = "balcony-air-quality",
        .name = "Air Quality - Balcony",
        .device_class = NULL,
        .unit_of_measurement = "ppm",
        .channel = ADC_CHANNEL_4,
        .attenuation = ADC_ATTEN_DB_12,
        .conversion = ADC_CONVERSION_MQ135_PPM,
        .fast_sample = false,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .state_key = "balcony/light-intensity",
        .entity_key = "balcony-light-intensity",
        .name = "Light Intensity - Balcony",
        .device_class = "illuminance",
        .unit_of_measurement = "lx",
        .channel = ADC_CHANNEL_5,
        .attenuation = ADC_ATTEN_DB_12,
        .conversion = ADC_CONVERSION_ILLUMINANCE_LUX,
        .fast_sample = false,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .state_key = "balcony/rain-index",
        .entity_key = "balcony-rain-index",
        .name = "Rain Index - Balcony",
        .device_class = NULL,
        .unit_of_measurement = "%",
        .channel = ADC_CHANNEL_6,
        .attenuation = ADC_ATTEN_DB_12,
        .conversion = ADC_CONVERSION_RAIN_PERCENT,
        .fast_sample = false,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
    {
        .state_key = "kitchen/water-level",
        .entity_key = "kitchen-water-level",
        .name = "Water Level - Kitchen",
        .device_class = NULL,
        .unit_of_measurement = "%",
        .channel = ADC_CHANNEL_7,
        .attenuation = ADC_ATTEN_DB_12,
        .conversion = ADC_CONVERSION_WATER_LEVEL_PERCENT,
        .fast_sample = false,
        .include_full_device_info = false,
        .state_group = HA_DISCOVERY_INVALID_STATE_GROUP_HANDLE,
    },
};

static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t s_cali_handles[ADC_ATTEN_DB_12 + 1];
static bool s_cali_ready[ADC_ATTEN_DB_12 + 1];
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static esp_err_t create_calibration(adc_atten_t attenuation)
{
    if (attenuation > ADC_ATTEN_DB_12) return ESP_ERR_INVALID_ARG;
    if (s_cali_ready[attenuation]) return ESP_OK;

    const adc_cali_curve_fitting_config_t config = {
        .unit_id = ADC_UNIT_1,
        .atten = attenuation,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&config, &s_cali_handles[attenuation]);
    if (err != ESP_OK) return err;

    s_cali_ready[attenuation] = true;
    return ESP_OK;
}

static esp_err_t read_median_raw(adc_channel_t channel, int *raw_value)
{
    if (raw_value == NULL) return ESP_ERR_INVALID_ARG;

    int samples[ADC_MEDIAN_SAMPLE_COUNT];
    for (size_t i = 0; i < ADC_MEDIAN_SAMPLE_COUNT; ++i) {
        esp_err_t err = adc_oneshot_read(s_adc_unit, channel, &samples[i]);
        if (err != ESP_OK) return err;
    }

    // 做插入排序
    for (size_t i = 1; i < ADC_MEDIAN_SAMPLE_COUNT; ++i) {
        int current = samples[i];
        size_t previous = i;
        while (previous > 0 && samples[previous - 1] > current) {
            samples[previous] = samples[previous - 1];
            --previous;
        }
        samples[previous] = current;
    }

    // 得到中位数
    *raw_value = samples[ADC_MEDIAN_SAMPLE_COUNT / 2];
    return ESP_OK;
}

static esp_err_t read_median_millivolts(const adc_sensor_t *sensor, int *millivolts)
{
    int raw_value;
    esp_err_t err = read_median_raw(sensor->channel, &raw_value);
    if (err != ESP_OK) return err;

    if (sensor->attenuation > ADC_ATTEN_DB_12 || !s_cali_ready[sensor->attenuation]) return ESP_ERR_INVALID_STATE;

    return adc_cali_raw_to_voltage(s_cali_handles[sensor->attenuation], raw_value, millivolts);
}

static esp_err_t convert_value(adc_conversion_t conversion, int millivolts, float *value)
{
    if (value == NULL || millivolts < 0) return ESP_ERR_INVALID_ARG;

    const float voltage = (float)millivolts / 1000.0f;
    float result;

    switch (conversion) {
    case ADC_CONVERSION_KNOB_PERCENT:
        result = clampf((voltage - 0.100f) / (3.157f - 0.100f) * 100.0f, 0.0f, 100.0f);
        break;

    case ADC_CONVERSION_MQ2_PPM:
    case ADC_CONVERSION_MQ135_PPM: {
        if (voltage <= 0.0f || voltage >= 5.0f) return ESP_ERR_INVALID_STATE;
        const float resistance = (5.0f - voltage) / (voltage / 0.5f);
        if (resistance <= 0.0f) return ESP_ERR_INVALID_STATE;
        result = powf(11.5428f * 6.64f / resistance, 0.6549f);
        if (result < 0.0f) result = 0.0f;
        break;
    }

    case ADC_CONVERSION_UV_INDEX:
        if (voltage < 0.050f) result = 0.0f;
        else if (voltage <= 0.227f) result = 1.0f;
        else if (voltage <= 0.318f) result = 2.0f;
        else if (voltage <= 0.408f) result = 3.0f;
        else if (voltage <= 0.503f) result = 4.0f;
        else if (voltage <= 0.606f) result = 5.0f;
        else if (voltage <= 0.696f) result = 6.0f;
        else if (voltage <= 0.795f) result = 7.0f;
        else if (voltage <= 0.881f) result = 8.0f;
        else if (voltage <= 0.976f) result = 9.0f;
        else result = 10.0f;
        break;

    case ADC_CONVERSION_ILLUMINANCE_LUX:
        if (voltage >= 3.290f || voltage <= 0.0f) result = 999.0f;
        else {
            const float resistance = voltage / (3.3f - voltage) * 10000.0f;
            result = 40000.0f * powf(resistance, -0.6021f);
            result = clampf(result, 0.0f, 999.0f);
        }
        break;

    case ADC_CONVERSION_RAIN_PERCENT:
        result = clampf((3.157f - voltage) / (3.157f - 1.500f) * 100.0f, 0.0f, 100.0f);
        break;

    case ADC_CONVERSION_WATER_LEVEL_PERCENT:
        result = clampf((voltage - 0.100f) / (3.157f - 0.100f) * 100.0f, 0.0f, 100.0f);
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (!isfinite(result)) return ESP_ERR_INVALID_STATE;
    *value = result;
    return ESP_OK;
}

static esp_err_t encode_adc_state(char *payload, size_t payload_size, void *context)
{
    adc_sensor_t *sensor = context;
    portENTER_CRITICAL(&s_state_lock);
    const bool valid = sensor->valid;
    const float value = sensor->value;
    portEXIT_CRITICAL(&s_state_lock);

    if (!valid) return ESP_ERR_INVALID_STATE;

    const int written = snprintf(payload, payload_size, "%.0f", (double)value);
    return written >= 0 && (size_t)written < payload_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void request_state_publish(adc_sensor_t *sensor, float value, bool valid)
{
    esp_err_t err = ha_discovery_publish_state_group(sensor->state_group);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s state publish request failed: %s", sensor->name, esp_err_to_name(err));
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (valid) {
        sensor->has_published_value = true;
        sensor->last_published_value = value;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void sample_sensor(adc_sensor_t *sensor)
{
    int millivolts;
    float value = 0.0f;
    esp_err_t err = read_median_millivolts(sensor, &millivolts);
    if (err == ESP_OK) err = convert_value(sensor->conversion, millivolts, &value);

    bool should_publish;
    bool valid = err == ESP_OK;
    portENTER_CRITICAL(&s_state_lock);
    if (valid) {
        sensor->valid = true;
        sensor->value = value;
        sensor->unavailable_reported = false;
        should_publish = !sensor->fast_sample || !sensor->has_published_value
                         || fabsf(value - sensor->last_published_value)
                                >= ADC_KNOB_PUBLISH_DEADBAND_PERCENT;
    } else {
        should_publish = !sensor->unavailable_reported;
        sensor->valid = false;
        sensor->unavailable_reported = true;
        sensor->has_published_value = false;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!valid) {
        ESP_LOGW(TAG, "%s sample failed: %s", sensor->name, esp_err_to_name(err));
    }
    if (should_publish) request_state_publish(sensor, value, valid);
}

static void adc_fast_sampling_task(void *argument)
{
    (void)argument;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        sample_sensor(&s_adc_sensors[0]);
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(ADC_FAST_SAMPLE_INTERVAL_MS));
    }
}

static void adc_slow_sampling_task(void *argument)
{
    (void)argument;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        for (size_t i = 1; i < ADC_SENSOR_COUNT; ++i) sample_sensor(&s_adc_sensors[i]);
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(ADC_SLOW_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t adc_init(void)
{
    if (s_initialized) return ESP_OK;

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &s_adc_unit);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < ADC_SENSOR_COUNT; ++i) {
        const adc_oneshot_chan_cfg_t channel_config = {
            .atten = s_adc_sensors[i].attenuation,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_oneshot_config_channel(s_adc_unit, s_adc_sensors[i].channel, &channel_config);
        if (err != ESP_OK) return err;

        err = create_calibration(s_adc_sensors[i].attenuation);
        if (err != ESP_OK) return err;

        const ha_discovery_state_group_config_t state_group_config = {
            .state_key = s_adc_sensors[i].state_key,
            .encode_payload = encode_adc_state,
            .context = &s_adc_sensors[i],
        };
        err = ha_discovery_register_state_group(&state_group_config, &s_adc_sensors[i].state_group);
        if (err != ESP_OK) return err;

        const ha_discovery_sensor_config_t entity_config = {
            .entity_key = s_adc_sensors[i].entity_key,
            .name = s_adc_sensors[i].name,
            .device_class = s_adc_sensors[i].device_class,
            .unit_of_measurement = s_adc_sensors[i].unit_of_measurement,
            .entity_category = NULL,
            .value_template = NULL,
            .state_group = s_adc_sensors[i].state_group,
            .include_full_device_info = s_adc_sensors[i].include_full_device_info,
        };
        ha_discovery_sensor_handle_t ignored_entity;
        err = ha_discovery_register_sensor(&entity_config, &ignored_entity);
        if (err != ESP_OK) return err;
    }

    if (xTaskCreate(adc_fast_sampling_task, "adc_fast", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(adc_slow_sampling_task, "adc_slow", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    s_initialized = true;
    ESP_LOGI(TAG, "ADC sensors initialized: 1 fast channel and 6 filtered slow channels");
    return ESP_OK;
}
