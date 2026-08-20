#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化卧室风扇、厨房风扇和厨房蜂鸣器的 MQTT Switch 执行器。 */
esp_err_t output_switches_init(void);

#ifdef __cplusplus
}
#endif
