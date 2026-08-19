# 传感器业务组件：采样任务和 Home Assistant MQTT 实体发布。
idf_component_register(
    SRCS
        "adc.c"
        "binary.c"
        "dht11.c"
        "sensors.c"
    INCLUDE_DIRS
        "include"

    REQUIRES
        dht
        driver
        esp_adc
        freertos
        ha_discovery
)
