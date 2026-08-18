# 传感器业务组件：采样任务和 Home Assistant MQTT 实体发布。
idf_component_register(
    SRCS
        "sensors.c"
    INCLUDE_DIRS
        "include"

    REQUIRES
        dht
        freertos
        ha_discovery
)
