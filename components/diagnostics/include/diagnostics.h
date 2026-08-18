#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化主控诊断实体，例如 Wi-Fi RSSI。 */
esp_err_t diagnostics_init(void);

#ifdef __cplusplus
}
#endif
