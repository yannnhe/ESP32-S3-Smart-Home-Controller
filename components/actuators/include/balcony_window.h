#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化阳台窗舵机及其 MQTT Switch；每次启动均执行一次物理关窗。 */
esp_err_t balcony_window_init(void);

#ifdef __cplusplus
}
#endif
