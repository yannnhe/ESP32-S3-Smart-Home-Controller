#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO1 亮度旋钮值变化时的快速通知；回调只能非阻塞地投递命令。 */
typedef void (*adc_knob_listener_t)(float percent, void *context);

esp_err_t adc_init(void);

/** 注册唯一的亮度旋钮监听器。 */
esp_err_t adc_register_knob_listener(adc_knob_listener_t listener, void *context);

/** 读取当前已缓存的亮度旋钮百分比；尚无有效采样值时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t adc_get_knob_percent(float *percent);

#ifdef __cplusplus
}
#endif
