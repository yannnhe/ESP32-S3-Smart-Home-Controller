#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化环境传感器业务组件。 */
esp_err_t sensors_init(void);

#ifdef __cplusplus
}
#endif
