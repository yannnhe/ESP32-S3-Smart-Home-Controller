#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化客厅 WS2812B 灯带、本地按键和 MQTT Light 命令处理。 */
esp_err_t living_room_light_init(void);

#ifdef __cplusplus
}
#endif
