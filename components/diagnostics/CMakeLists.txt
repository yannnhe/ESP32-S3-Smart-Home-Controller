# 主控诊断实体。
idf_component_register(
    SRCS
        "diagnostics.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        freertos
        ha_discovery
        network
)
