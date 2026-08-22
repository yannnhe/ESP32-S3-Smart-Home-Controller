#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化固定 HTTP 源的 OTA 服务、HA 实体和升级后健康检查。
 *
 * 固件地址只读取 menuconfig 的 OTA_SERVICE_FIRMWARE_URL；MQTT 仅接受 START 命令，
 * 不接受 URL 或其他下载参数。
 */
esp_err_t ota_service_init(void);

#ifdef __cplusplus
}
#endif
