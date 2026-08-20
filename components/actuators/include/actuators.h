#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化所有执行器业务组件。 */
esp_err_t actuators_init(void);

#ifdef __cplusplus
}
#endif
